#include "MetadataStorageFromPlainObjectStorage.h"
#include <Disks/IDisk.h>
#include <Disks/ObjectStorages/InMemoryPathMap.h>
#include <Disks/ObjectStorages/MetadataStorageFromPlainObjectStorageOperations.h>
#include <Disks/ObjectStorages/StaticDirectoryIterator.h>
#include <Disks/ObjectStorages/StoredObject.h>

#include <Common/filesystemHelpers.h>

#include <filesystem>
#include <tuple>
#include <unordered_set>


namespace DB
{

namespace ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
}

namespace
{

std::filesystem::path normalizeDirectoryPath(const std::filesystem::path & path)
{
    return path / "";
}

}

MetadataStorageFromPlainObjectStorage::MetadataStorageFromPlainObjectStorage(ObjectStoragePtr object_storage_, String storage_path_prefix_)
    : object_storage(object_storage_)
    , storage_path_prefix(std::move(storage_path_prefix_))
{
}

MetadataTransactionPtr MetadataStorageFromPlainObjectStorage::createTransaction()
{
    return std::make_shared<MetadataStorageFromPlainObjectStorageTransaction>(*this, object_storage);
}

const std::string & MetadataStorageFromPlainObjectStorage::getPath() const
{
    return storage_path_prefix;
}

bool MetadataStorageFromPlainObjectStorage::existsFile(const std::string & path) const
{
    ObjectStorageKey object_key = object_storage->generateObjectKeyForPath(path, std::nullopt /* key_prefix */);
    StoredObject object(object_key.serialize(), path);
    return object_storage->exists(object);
}

bool MetadataStorageFromPlainObjectStorage::existsDirectory(const std::string & path) const
{
    auto key_prefix = object_storage->generateObjectKeyForPath(path, std::nullopt /* key_prefix */).serialize();
    auto directory = std::filesystem::path(std::move(key_prefix)) / "";
    return object_storage->existsOrHasAnyChild(directory);
}

bool MetadataStorageFromPlainObjectStorage::existsFileOrDirectory(const std::string & path) const
{
    /// NOTE: exists() cannot be used here since it works only for existing
    /// key, and does not work for some intermediate path.
    auto key_prefix = object_storage->generateObjectKeyForPath(path, std::nullopt /* key_prefix */).serialize();
    return object_storage->existsOrHasAnyChild(key_prefix);
}


uint64_t MetadataStorageFromPlainObjectStorage::getFileSize(const String & path) const
{
    if (auto res = getFileSizeIfExists(path))
        return *res;
    throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "File {} does not exist on plain object storage", path);
}

std::optional<uint64_t> MetadataStorageFromPlainObjectStorage::getFileSizeIfExists(const String & path) const
{
    auto object_key = object_storage->generateObjectKeyForPath(path, std::nullopt /* key_prefix */);
    auto metadata = object_storage->tryGetObjectMetadata(object_key.serialize());
    if (metadata)
        return metadata->size_bytes;
    return std::nullopt;
}

std::vector<std::string> MetadataStorageFromPlainObjectStorage::listDirectory(const std::string & path) const
{
    auto key_prefix = object_storage->generateObjectKeyForPath(path, std::nullopt /* key_prefix */).serialize();

    RelativePathsWithMetadata files;
    std::string absolute_key = key_prefix;
    if (!absolute_key.ends_with('/'))
        absolute_key += '/';

    object_storage->listObjects(absolute_key, files, 0);

    std::unordered_set<std::string> result;
    for (const auto & elem : files)
    {
        const auto & p = elem->relative_path;
        chassert(p.find(absolute_key) == 0);
        const auto child_pos = absolute_key.size();
        /// string::npos is ok.
        const auto slash_pos = p.find('/', child_pos);
        if (slash_pos == std::string::npos)
            result.emplace(p.substr(child_pos));
        else
            result.emplace(p.substr(child_pos, slash_pos - child_pos));
    }
    return std::vector<std::string>(std::make_move_iterator(result.begin()), std::make_move_iterator(result.end()));
}

DirectoryIteratorPtr MetadataStorageFromPlainObjectStorage::iterateDirectory(const std::string & path) const
{
    /// Required for MergeTree
    auto paths = listDirectory(path);
    /// Prepend path, since iterateDirectory() includes path, unlike listDirectory()
    std::for_each(paths.begin(), paths.end(), [&](auto & child) { child = fs::path(path) / child; });
    std::vector<std::filesystem::path> fs_paths(paths.begin(), paths.end());
    return std::make_unique<StaticDirectoryIterator>(std::move(fs_paths));
}

StoredObjects MetadataStorageFromPlainObjectStorage::getStorageObjects(const std::string & path) const
{
    size_t object_size = getFileSize(path);
    auto object_key = object_storage->generateObjectKeyForPath(path, std::nullopt /* key_prefix */);
    return {StoredObject(object_key.serialize(), path, object_size)};
}

std::optional<StoredObjects> MetadataStorageFromPlainObjectStorage::getStorageObjectsIfExist(const std::string & path) const
{
    if (auto object_size = getFileSizeIfExists(path))
    {
        auto object_key = object_storage->generateObjectKeyForPath(path, std::nullopt /* key_prefix */);
        return StoredObjects{StoredObject(object_key.serialize(), path, *object_size)};
    }
    return std::nullopt;
}

const IMetadataStorage & MetadataStorageFromPlainObjectStorageTransaction::getStorageForNonTransactionalReads() const
{
    return metadata_storage;
}

void MetadataStorageFromPlainObjectStorageTransaction::unlinkFile(const std::string & path)
{
    auto object_key = metadata_storage.object_storage->generateObjectKeyForPath(path, std::nullopt /* key_prefix */);
    auto object = StoredObject(object_key.serialize());
    metadata_storage.object_storage->removeObject(object);
}

void MetadataStorageFromPlainObjectStorageTransaction::removeDirectory(const std::string & path)
{
    if (metadata_storage.object_storage->isWriteOnce())
    {
        for (auto it = metadata_storage.iterateDirectory(path); it->isValid(); it->next())
            metadata_storage.object_storage->removeObject(StoredObject(it->path()));
    }
    else
    {
        addOperation(std::make_unique<MetadataStorageFromPlainObjectStorageRemoveDirectoryOperation>(
            normalizeDirectoryPath(path), *metadata_storage.getPathMap(), object_storage, metadata_storage.getMetadataKeyPrefix()));
    }
}

void MetadataStorageFromPlainObjectStorageTransaction::createDirectory(const std::string & path)
{
    if (metadata_storage.object_storage->isWriteOnce())
        return;

    auto normalized_path = normalizeDirectoryPath(path);
    auto op = std::make_unique<MetadataStorageFromPlainObjectStorageCreateDirectoryOperation>(
        std::move(normalized_path),
        *metadata_storage.getPathMap(),
        object_storage,
        metadata_storage.getMetadataKeyPrefix());
    addOperation(std::move(op));
}

void MetadataStorageFromPlainObjectStorageTransaction::createDirectoryRecursive(const std::string & path)
{
    createDirectory(path);
}

void MetadataStorageFromPlainObjectStorageTransaction::moveDirectory(const std::string & path_from, const std::string & path_to)
{
    if (metadata_storage.object_storage->isWriteOnce())
        throwNotImplemented();

    addOperation(std::make_unique<MetadataStorageFromPlainObjectStorageMoveDirectoryOperation>(
        normalizeDirectoryPath(path_from),
        normalizeDirectoryPath(path_to),
        *metadata_storage.getPathMap(),
        object_storage,
        metadata_storage.getMetadataKeyPrefix()));
}

void MetadataStorageFromPlainObjectStorageTransaction::addBlobToMetadata(
    const std::string &, ObjectStorageKey /* object_key */, uint64_t /* size_in_bytes */)
{
    /// Noop, local metadata files is only one file, it is the metadata file itself.
}

UnlinkMetadataFileOperationOutcomePtr MetadataStorageFromPlainObjectStorageTransaction::unlinkMetadata(const std::string &)
{
    /// No hardlinks, so will always remove file.
    return std::make_shared<UnlinkMetadataFileOperationOutcome>(UnlinkMetadataFileOperationOutcome{0});
}

void MetadataStorageFromPlainObjectStorageTransaction::commit()
{
    MetadataOperationsHolder::commitImpl(metadata_storage.metadata_mutex);
}
}
