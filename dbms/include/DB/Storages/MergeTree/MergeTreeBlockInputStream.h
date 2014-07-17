#pragma once

#include <DB/DataStreams/IProfilingBlockInputStream.h>
#include <DB/Storages/MergeTree/MergeTreeData.h>
#include <DB/Storages/MergeTree/PKCondition.h>
#include <DB/Storages/MergeTree/MergeTreeReader.h>


namespace DB
{

/// Для чтения из одного куска. Для чтения сразу из многих, Storage использует сразу много таких объектов.
class MergeTreeBlockInputStream : public IProfilingBlockInputStream
{
public:
	MergeTreeBlockInputStream(const String & path_,	/// Путь к куску
		size_t block_size_, const Names & column_names_,
		MergeTreeData & storage_, const MergeTreeData::DataPartPtr & owned_data_part_,
		const MarkRanges & mark_ranges_, bool use_uncompressed_cache_,
		ExpressionActionsPtr prewhere_actions_, String prewhere_column_)
		:
		path(path_), block_size(block_size_), column_names(column_names_),
		storage(storage_), owned_data_part(owned_data_part_),
		part_columns_lock(new Poco::ScopedReadRWLock(owned_data_part->columns_lock)),
		all_mark_ranges(mark_ranges_), remaining_mark_ranges(mark_ranges_),
		use_uncompressed_cache(use_uncompressed_cache_),
		prewhere_actions(prewhere_actions_), prewhere_column(prewhere_column_),
		log(&Logger::get("MergeTreeBlockInputStream"))
	{
		/// Под owned_data_part->columns_lock проверим, что все запрошенные столбцы в куске того же типа, что в таблице.
		/// Это может быть не так во время ALTER MODIFY.
		storage.check(owned_data_part->columns, column_names);

		std::reverse(remaining_mark_ranges.begin(), remaining_mark_ranges.end());

		if (prewhere_actions)
		{
			pre_column_names = prewhere_actions->getRequiredColumns();
			if (pre_column_names.empty())
				pre_column_names.push_back(column_names[0]);
			NameSet pre_name_set(pre_column_names.begin(), pre_column_names.end());
			/// Если выражение в PREWHERE - не столбец таблицы, не нужно отдавать наружу столбец с ним
			///  (от storage ожидают получить только столбцы таблицы).
			remove_prewhere_column = !pre_name_set.count(prewhere_column);
			Names post_column_names;
			for (size_t i = 0; i < column_names.size(); ++i)
			{
				if (!pre_name_set.count(column_names[i]))
					post_column_names.push_back(column_names[i]);
			}
			column_names = post_column_names;
		}
		column_name_set.insert(column_names.begin(), column_names.end());

		LOG_TRACE(log, "Reading " << all_mark_ranges.size() << " ranges from part " << owned_data_part->name
			<< ", up to " << (all_mark_ranges.back().end - all_mark_ranges.front().begin) * storage.index_granularity
			<< " rows starting from " << all_mark_ranges.front().begin * storage.index_granularity);
	}

	String getName() const { return "MergeTreeBlockInputStream"; }

	String getID() const
	{
		std::stringstream res;
		res << "MergeTree(" << path << ", columns";

		for (size_t i = 0; i < column_names.size(); ++i)
			res << ", " << column_names[i];

		res << ", marks";

		for (size_t i = 0; i < all_mark_ranges.size(); ++i)
			res << ", " << all_mark_ranges[i].begin << ", " << all_mark_ranges[i].end;

		res << ")";
		return res.str();
	}

protected:
	/// Будем вызывать progressImpl самостоятельно.
	void progress(size_t rows, size_t bytes) {}

	Block readImpl()
	{
		Block res;

		if (remaining_mark_ranges.empty())
			return res;

		if (!reader)
		{
			UncompressedCache * uncompressed_cache = use_uncompressed_cache ? storage.context.getUncompressedCache() : NULL;
			reader.reset(new MergeTreeReader(path, column_names, uncompressed_cache, storage, all_mark_ranges));
			if (prewhere_actions)
				pre_reader.reset(new MergeTreeReader(path, pre_column_names, uncompressed_cache, storage, all_mark_ranges));
		}

		if (prewhere_actions)
		{
			do
			{
				/// Прочитаем полный блок столбцов, нужных для вычисления выражения в PREWHERE.
				size_t space_left = std::max(1LU, block_size / storage.index_granularity);
				MarkRanges ranges_to_read;
				while (!remaining_mark_ranges.empty() && space_left)
				{
					MarkRange & range = remaining_mark_ranges.back();

					size_t marks_to_read = std::min(range.end - range.begin, space_left);
					pre_reader->readRange(range.begin, range.begin + marks_to_read, res);

					ranges_to_read.push_back(MarkRange(range.begin, range.begin + marks_to_read));
					space_left -= marks_to_read;
					range.begin += marks_to_read;
					if (range.begin == range.end)
						remaining_mark_ranges.pop_back();
				}
				progressImpl(res.rows(), res.bytes());
				pre_reader->fillMissingColumns(res);

				/// Вычислим выражение в PREWHERE.
				prewhere_actions->execute(res);

				ColumnPtr column = res.getByName(prewhere_column).column;
				if (remove_prewhere_column)
					res.erase(prewhere_column);

				size_t pre_bytes = res.bytes();

				/** Если фильтр - константа (например, написано PREWHERE 1),
				*  то либо вернём пустой блок, либо вернём блок без изменений.
				*/
				if (ColumnConstUInt8 * column_const = typeid_cast<ColumnConstUInt8 *>(&*column))
				{
					if (!column_const->getData())
					{
						res.clear();
						return res;
					}

					for (size_t i = 0; i < ranges_to_read.size(); ++i)
					{
						const MarkRange & range = ranges_to_read[i];
						reader->readRange(range.begin, range.end, res);
					}

					progressImpl(0, res.bytes() - pre_bytes);
				}
				else if (ColumnUInt8 * column_vec = typeid_cast<ColumnUInt8 *>(&*column))
				{
					size_t index_granularity = storage.index_granularity;

					const IColumn::Filter & pre_filter = column_vec->getData();
					IColumn::Filter post_filter(pre_filter.size());

					/// Прочитаем в нужных отрезках остальные столбцы и составим для них свой фильтр.
					size_t pre_filter_pos = 0;
					size_t post_filter_pos = 0;
					for (size_t i = 0; i < ranges_to_read.size(); ++i)
					{
						const MarkRange & range = ranges_to_read[i];

						size_t begin = range.begin;
						size_t pre_filter_begin_pos = pre_filter_pos;
						for (size_t mark = range.begin; mark <= range.end; ++mark)
						{
							UInt8 nonzero = 0;
							if (mark != range.end)
							{
								size_t limit = std::min(pre_filter.size(), pre_filter_pos + index_granularity);
								for (size_t row = pre_filter_pos; row < limit; ++row)
									nonzero |= pre_filter[row];
							}
							if (!nonzero)
							{
								if (mark > begin)
								{
									memcpy(
										&post_filter[post_filter_pos],
										&pre_filter[pre_filter_begin_pos],
										pre_filter_pos - pre_filter_begin_pos);
									post_filter_pos += pre_filter_pos - pre_filter_begin_pos;
									reader->readRange(begin, mark, res);
								}
								begin = mark + 1;
								pre_filter_begin_pos = std::min(pre_filter_pos + index_granularity, pre_filter.size());
							}
							if (mark < range.end)
								pre_filter_pos = std::min(pre_filter_pos + index_granularity, pre_filter.size());
						}
					}

					if (!post_filter_pos)
					{
						res.clear();
						continue;
					}

					progressImpl(0, res.bytes() - pre_bytes);

					post_filter.resize(post_filter_pos);

					/// Отфильтруем столбцы, относящиеся к PREWHERE, используя pre_filter,
					///  остальные столбцы - используя post_filter.
					size_t rows = 0;
					for (size_t i = 0; i < res.columns(); ++i)
					{
						ColumnWithNameAndType & column = res.getByPosition(i);
						if (column.name == prewhere_column && res.columns() > 1)
							continue;
						column.column = column.column->filter(column_name_set.count(column.name) ? post_filter : pre_filter);
						rows = column.column->size();
					}

					/// Заменим столбец со значением условия из PREWHERE на константу.
					if (!remove_prewhere_column)
						res.getByName(prewhere_column).column = new ColumnConstUInt8(rows, 1);
				}
				else
					throw Exception("Illegal type " + column->getName() + " of column for filter. Must be ColumnUInt8 or ColumnConstUInt8.", ErrorCodes::ILLEGAL_TYPE_OF_COLUMN_FOR_FILTER);

				reader->fillMissingColumns(res);
			}
			while (!remaining_mark_ranges.empty() && !res && !isCancelled());
		}
		else
		{
			size_t space_left = std::max(1LU, block_size / storage.index_granularity);
			while (!remaining_mark_ranges.empty() && space_left)
			{
				MarkRange & range = remaining_mark_ranges.back();

				size_t marks_to_read = std::min(range.end - range.begin, space_left);
				reader->readRange(range.begin, range.begin + marks_to_read, res);

				space_left -= marks_to_read;
				range.begin += marks_to_read;
				if (range.begin == range.end)
					remaining_mark_ranges.pop_back();
			}

			progressImpl(res.rows(), res.bytes());

			reader->fillMissingColumns(res);
		}

		if (remaining_mark_ranges.empty())
		{
			/** Закрываем файлы (ещё до уничтожения объекта).
				* Чтобы при создании многих источников, но одновременном чтении только из нескольких,
				*  буферы не висели в памяти.
				*/
			reader.reset();
			pre_reader.reset();
			part_columns_lock.reset();
			owned_data_part.reset();
		}

		return res;
	}

private:
	const String path;
	size_t block_size;
	Names column_names;
	NameSet column_name_set;
	Names pre_column_names;
	MergeTreeData & storage;
	MergeTreeData::DataPartPtr owned_data_part;	/// Кусок не будет удалён, пока им владеет этот объект.
	std::unique_ptr<Poco::ScopedReadRWLock> part_columns_lock; /// Не дадим изменить список столбцов куска, пока мы из него читаем.
	MarkRanges all_mark_ranges; /// В каких диапазонах засечек читать. В порядке возрастания номеров.
	MarkRanges remaining_mark_ranges; /// В каких диапазонах засечек еще не прочли.
									  /// В порядке убывания номеров, чтобы можно было выбрасывать из конца.
	bool use_uncompressed_cache;
	std::unique_ptr<MergeTreeReader> reader;
	std::unique_ptr<MergeTreeReader> pre_reader;
	ExpressionActionsPtr prewhere_actions;
	String prewhere_column;
	bool remove_prewhere_column;

	Logger * log;
};

}
