#include "longtail_memstorage.h"

#include "../../src/longtail.h"
#include "../longtail_platform.h"
#include "../../src/ext/stb_ds.h"

#include <errno.h>

static const uint32_t Prime = 0x01000193;
static const uint32_t Seed  = 0x811C9DC5;

static uint32_t fnv1a(const void* data, uint32_t numBytes)
{
    uint32_t hash = Seed;
    const unsigned char* ptr = (const unsigned char*)data;
    while (numBytes--)
    {
        hash = ((*ptr++) ^ hash) * Prime;
    }
    return hash;
}

struct PathEntry
{
    char* m_FileName;
    uint32_t m_ParentHash;
    uint8_t* m_Content;
};

struct Lookup
{
    uint32_t key;
    uint32_t value;
};

struct InMemStorageAPI
{
    struct Longtail_StorageAPI m_InMemStorageAPI;
    struct Lookup* m_PathHashToContent;
    struct PathEntry* m_PathEntries;
    HLongtail_SpinLock m_SpinLock;
};

static void InMemStorageAPI_Dispose(struct Longtail_API* storage_api)
{
    struct InMemStorageAPI* in_mem_storage_api = (struct InMemStorageAPI*)storage_api;
    size_t c = (size_t)arrlen(in_mem_storage_api->m_PathEntries);
    while(c--)
    {
        struct PathEntry* path_entry = &in_mem_storage_api->m_PathEntries[c];
        Longtail_Free(path_entry->m_FileName);
        path_entry->m_FileName = 0;
        arrfree(path_entry->m_Content);
        path_entry->m_Content = 0;
    }
    Longtail_DeleteSpinLock(in_mem_storage_api->m_SpinLock);
    hmfree(in_mem_storage_api->m_PathHashToContent);
    in_mem_storage_api->m_PathHashToContent = 0;
    arrfree(in_mem_storage_api->m_PathEntries);
    in_mem_storage_api->m_PathEntries = 0;
    Longtail_Free(storage_api);
}

static uint32_t InMemStorageAPI_GetPathHash(const char* path)
{
    return fnv1a((void*)path, (uint32_t)strlen(path));
}

static int InMemStorageAPI_OpenReadFile(struct Longtail_StorageAPI* storage_api, const char* path, Longtail_StorageAPI_HOpenFile* out_open_file)
{
    struct InMemStorageAPI* instance = (struct InMemStorageAPI*)storage_api;
    Longtail_LockSpinLock(instance->m_SpinLock);
    uint32_t path_hash = InMemStorageAPI_GetPathHash(path);
    intptr_t it = hmgeti(instance->m_PathHashToContent, path_hash);
    if (it != -1)
    {
        Longtail_UnlockSpinLock(instance->m_SpinLock);
        *out_open_file = (Longtail_StorageAPI_HOpenFile)(uintptr_t)path_hash;
        return 0;
    }
    Longtail_UnlockSpinLock(instance->m_SpinLock);
    return ENOENT;
}

static int InMemStorageAPI_GetSize(struct Longtail_StorageAPI* storage_api, Longtail_StorageAPI_HOpenFile f, uint64_t* out_size)
{
    struct InMemStorageAPI* instance = (struct InMemStorageAPI*)storage_api;
    Longtail_LockSpinLock(instance->m_SpinLock);
    uint32_t path_hash = (uint32_t)(uintptr_t)f;
    intptr_t it = hmgeti(instance->m_PathHashToContent, path_hash);
    if (it == -1) {
        Longtail_UnlockSpinLock(instance->m_SpinLock);
        return ENOENT;
    }
    struct PathEntry* path_entry = (struct PathEntry*)&instance->m_PathEntries[instance->m_PathHashToContent[it].value];
    uint64_t size = (uint64_t)arrlen(path_entry->m_Content);
    Longtail_UnlockSpinLock(instance->m_SpinLock);
    *out_size = size;
    return 0;
}

static int InMemStorageAPI_Read(struct Longtail_StorageAPI* storage_api, Longtail_StorageAPI_HOpenFile f, uint64_t offset, uint64_t length, void* output)
{
    struct InMemStorageAPI* instance = (struct InMemStorageAPI*)storage_api;
    Longtail_LockSpinLock(instance->m_SpinLock);
    uint32_t path_hash = (uint32_t)(uintptr_t)f;
    intptr_t it = hmgeti(instance->m_PathHashToContent, path_hash);
    if (it == -1) {
        Longtail_UnlockSpinLock(instance->m_SpinLock);
        return ENOENT;
    }
    struct PathEntry* path_entry = (struct PathEntry*)&instance->m_PathEntries[instance->m_PathHashToContent[it].value];
    if ((ptrdiff_t)(offset + length) > arrlen(path_entry->m_Content))
    {
        Longtail_UnlockSpinLock(instance->m_SpinLock);
        return EIO;
    }
    memcpy(output, &path_entry->m_Content[offset], length);
    Longtail_UnlockSpinLock(instance->m_SpinLock);
    return 0;
}

static uint32_t InMemStorageAPI_GetParentPathHash(struct InMemStorageAPI* instance, const char* path)
{
    const char* dir_path_begin = strrchr(path, '/');
    if (!dir_path_begin)
    {
        return 0;
    }
    size_t dir_length = (uintptr_t)dir_path_begin - (uintptr_t)path;
    char* dir_path = (char*)Longtail_Alloc(dir_length + 1);
    strncpy(dir_path, path, dir_length);
    dir_path[dir_length] = '\0';
    uint32_t hash = InMemStorageAPI_GetPathHash(dir_path);
    Longtail_Free(dir_path);
    return hash;
}

static const char* InMemStorageAPI_GetFileNamePart(const char* path)
{
    const char* file_name = strrchr(path, '/');
    if (file_name == 0)
    {
        return path;
    }
    return &file_name[1];
}

static int InMemStorageAPI_OpenWriteFile(struct Longtail_StorageAPI* storage_api, const char* path, uint64_t initial_size, Longtail_StorageAPI_HOpenFile* out_open_file)
{
    struct InMemStorageAPI* instance = (struct InMemStorageAPI*)storage_api;
    Longtail_LockSpinLock(instance->m_SpinLock);
    uint32_t parent_path_hash = InMemStorageAPI_GetParentPathHash(instance, path);
    if (parent_path_hash != 0 && hmgeti(instance->m_PathHashToContent, parent_path_hash) == -1)
    {
        Longtail_UnlockSpinLock(instance->m_SpinLock);
        return ENOENT;
    }
    uint32_t path_hash = InMemStorageAPI_GetPathHash(path);
    struct PathEntry* path_entry = 0;
    intptr_t it = hmgeti(instance->m_PathHashToContent, path_hash);
    if (it != -1)
    {
        path_entry = &instance->m_PathEntries[instance->m_PathHashToContent[it].value];
    }
    else
    {
        ptrdiff_t entry_index = arrlen(instance->m_PathEntries);
        arrsetlen(instance->m_PathEntries, (size_t)(entry_index + 1));
        path_entry = &instance->m_PathEntries[entry_index];
        path_entry->m_ParentHash = parent_path_hash;
        path_entry->m_FileName = Longtail_Strdup(InMemStorageAPI_GetFileNamePart(path));
        path_entry->m_Content = 0;
        hmput(instance->m_PathHashToContent, path_hash, (uint32_t)entry_index);
    }
    arrsetcap(path_entry->m_Content, initial_size == 0 ? 16 : (uint32_t)initial_size);
    arrsetlen(path_entry->m_Content, (uint32_t)initial_size);
    Longtail_UnlockSpinLock(instance->m_SpinLock);
    *out_open_file = (Longtail_StorageAPI_HOpenFile)(uintptr_t)path_hash;
    return 0;
}

static int InMemStorageAPI_Write(struct Longtail_StorageAPI* storage_api, Longtail_StorageAPI_HOpenFile f, uint64_t offset, uint64_t length, const void* input)
{
    struct InMemStorageAPI* instance = (struct InMemStorageAPI*)storage_api;
    Longtail_LockSpinLock(instance->m_SpinLock);
    uint32_t path_hash = (uint32_t)(uintptr_t)f;
    intptr_t it = hmgeti(instance->m_PathHashToContent, path_hash);
    if (it == -1)
    {
        Longtail_UnlockSpinLock(instance->m_SpinLock);
        return ENOENT;
    }
    struct PathEntry* path_entry = &instance->m_PathEntries[instance->m_PathHashToContent[it].value];
    ptrdiff_t size = arrlen(path_entry->m_Content);
    if ((ptrdiff_t)offset > size)
    {
        Longtail_UnlockSpinLock(instance->m_SpinLock);
        return EIO;
    }
    if ((ptrdiff_t)(offset + length) > size)
    {
        size = offset + length;
    }
    arrsetcap(path_entry->m_Content, size == 0 ? 16 : (uint32_t)size);
    arrsetlen(path_entry->m_Content, (uint32_t)size);
    memcpy(&(path_entry->m_Content)[offset], input, length);
    Longtail_UnlockSpinLock(instance->m_SpinLock);
    return 0;
}

static int InMemStorageAPI_SetSize(struct Longtail_StorageAPI* storage_api, Longtail_StorageAPI_HOpenFile f, uint64_t length)
{
    struct InMemStorageAPI* instance = (struct InMemStorageAPI*)storage_api;
    Longtail_LockSpinLock(instance->m_SpinLock);
    uint32_t path_hash = (uint32_t)(uintptr_t)f;
    intptr_t it = hmgeti(instance->m_PathHashToContent, path_hash);
    if (it == -1)
    {
        Longtail_UnlockSpinLock(instance->m_SpinLock);
        return ENOENT;
    }
    struct PathEntry* path_entry = &instance->m_PathEntries[instance->m_PathHashToContent[it].value];
    arrsetlen(path_entry->m_Content, (uint32_t)length);
    Longtail_UnlockSpinLock(instance->m_SpinLock);
    return 0;
}

static void InMemStorageAPI_CloseFile(struct Longtail_StorageAPI* storage_api, Longtail_StorageAPI_HOpenFile f)
{
}

static int InMemStorageAPI_CreateDir(struct Longtail_StorageAPI* storage_api, const char* path)
{
    struct InMemStorageAPI* instance = (struct InMemStorageAPI*)storage_api;
    Longtail_LockSpinLock(instance->m_SpinLock);
    uint32_t parent_path_hash = InMemStorageAPI_GetParentPathHash(instance, path);
    if (parent_path_hash != 0 && hmgeti(instance->m_PathHashToContent, parent_path_hash) == -1)
    {
        Longtail_UnlockSpinLock(instance->m_SpinLock);
        return ENOENT;
    }
    uint32_t path_hash = InMemStorageAPI_GetPathHash(path);
    intptr_t source_path_ptr = hmgeti(instance->m_PathHashToContent, path_hash);
    if (source_path_ptr != -1)
    {
        struct PathEntry* source_entry = &instance->m_PathEntries[instance->m_PathHashToContent[source_path_ptr].value];
        if (source_entry->m_Content == 0)
        {
            Longtail_UnlockSpinLock(instance->m_SpinLock);
            return 0;
        }
        Longtail_UnlockSpinLock(instance->m_SpinLock);
        return EIO;
    }

    ptrdiff_t entry_index = arrlen(instance->m_PathEntries);
    arrsetlen(instance->m_PathEntries, (size_t)(entry_index + 1));
    struct PathEntry* path_entry = &instance->m_PathEntries[entry_index];
    path_entry->m_ParentHash = parent_path_hash;
    path_entry->m_FileName = Longtail_Strdup(InMemStorageAPI_GetFileNamePart(path));
    path_entry->m_Content = 0;
    hmput(instance->m_PathHashToContent, path_hash, (uint32_t)entry_index);
    Longtail_UnlockSpinLock(instance->m_SpinLock);
    return 0;
}

static int InMemStorageAPI_RenameFile(struct Longtail_StorageAPI* storage_api, const char* source_path, const char* target_path)
{
    struct InMemStorageAPI* instance = (struct InMemStorageAPI*)storage_api;
    Longtail_LockSpinLock(instance->m_SpinLock);
    uint32_t source_path_hash = InMemStorageAPI_GetPathHash(source_path);
    intptr_t source_path_ptr = hmgeti(instance->m_PathHashToContent, source_path_hash);
    if (source_path_ptr == -1)
    {
        Longtail_UnlockSpinLock(instance->m_SpinLock);
        return ENOENT;
    }
    struct PathEntry* source_entry = &instance->m_PathEntries[instance->m_PathHashToContent[source_path_ptr].value];

    uint32_t target_path_hash = InMemStorageAPI_GetPathHash(target_path);
    intptr_t target_path_ptr = hmgeti(instance->m_PathHashToContent, target_path_hash);
    if (target_path_ptr != -1)
    {
        Longtail_UnlockSpinLock(instance->m_SpinLock);
        return EEXIST;
    }
    source_entry->m_ParentHash = InMemStorageAPI_GetParentPathHash(instance, target_path);
    Longtail_Free(source_entry->m_FileName);
    source_entry->m_FileName = Longtail_Strdup(InMemStorageAPI_GetFileNamePart(target_path));
    hmput(instance->m_PathHashToContent, target_path_hash, instance->m_PathHashToContent[source_path_ptr].value);
    hmdel(instance->m_PathHashToContent, source_path_hash);
    Longtail_UnlockSpinLock(instance->m_SpinLock);
    return 0;
}

static char* InMemStorageAPI_ConcatPath(struct Longtail_StorageAPI* storage_api, const char* root_path, const char* sub_path)
{
    if (root_path[0] == 0)
    {
        return Longtail_Strdup(sub_path);
    }
    size_t path_len = strlen(root_path) + 1 + strlen(sub_path) + 1;
    char* path = (char*)Longtail_Alloc(path_len);
    strcpy(path, root_path);
    strcat(path, "/");
    strcat(path, sub_path);
    return path;
}

static int InMemStorageAPI_IsDir(struct Longtail_StorageAPI* storage_api, const char* path)
{
    struct InMemStorageAPI* instance = (struct InMemStorageAPI*)storage_api;
    Longtail_LockSpinLock(instance->m_SpinLock);
    uint32_t source_path_hash = InMemStorageAPI_GetPathHash(path);
    intptr_t source_path_ptr = hmgeti(instance->m_PathHashToContent, source_path_hash);
    if (source_path_ptr == -1)
    {
        Longtail_UnlockSpinLock(instance->m_SpinLock);
        return 0;
    }
    struct PathEntry* source_entry = &instance->m_PathEntries[instance->m_PathHashToContent[source_path_ptr].value];
    Longtail_UnlockSpinLock(instance->m_SpinLock);
    return source_entry->m_Content == 0;
}
static int InMemStorageAPI_IsFile(struct Longtail_StorageAPI* storage_api, const char* path)
{
    struct InMemStorageAPI* instance = (struct InMemStorageAPI*)storage_api;
    Longtail_LockSpinLock(instance->m_SpinLock);
    uint32_t path_hash = InMemStorageAPI_GetPathHash(path);
    intptr_t source_path_ptr = hmgeti(instance->m_PathHashToContent, path_hash);
    if (source_path_ptr == -1)
    {
        Longtail_UnlockSpinLock(instance->m_SpinLock);
        return 0;
    }
    struct PathEntry* source_entry = &instance->m_PathEntries[instance->m_PathHashToContent[source_path_ptr].value];
    Longtail_UnlockSpinLock(instance->m_SpinLock);
    return source_entry->m_Content != 0;
}

static int InMemStorageAPI_RemoveDir(struct Longtail_StorageAPI* storage_api, const char* path)
{
    struct InMemStorageAPI* instance = (struct InMemStorageAPI*)storage_api;
    Longtail_LockSpinLock(instance->m_SpinLock);
    uint32_t path_hash = InMemStorageAPI_GetPathHash(path);
    intptr_t source_path_ptr = hmgeti(instance->m_PathHashToContent, path_hash);
    if (source_path_ptr == -1)
    {
        Longtail_UnlockSpinLock(instance->m_SpinLock);
        return ENOENT;
    }
    struct PathEntry* path_entry = &instance->m_PathEntries[instance->m_PathHashToContent[source_path_ptr].value];
    if (path_entry->m_Content)
    {
        // Not a directory
        Longtail_UnlockSpinLock(instance->m_SpinLock);
        return EINVAL;
    }
    Longtail_Free(path_entry->m_FileName);
    path_entry->m_FileName = 0;
    arrfree(path_entry->m_Content);
    path_entry->m_Content = 0;
    path_entry->m_ParentHash = 0;
    hmdel(instance->m_PathHashToContent, path_hash);
    Longtail_UnlockSpinLock(instance->m_SpinLock);
    return 0;
}

static int InMemStorageAPI_RemoveFile(struct Longtail_StorageAPI* storage_api, const char* path)
{
    struct InMemStorageAPI* instance = (struct InMemStorageAPI*)storage_api;
    Longtail_LockSpinLock(instance->m_SpinLock);
    uint32_t path_hash = InMemStorageAPI_GetPathHash(path);
    intptr_t source_path_ptr = hmgeti(instance->m_PathHashToContent, path_hash);
    if (source_path_ptr == -1)
    {
        Longtail_UnlockSpinLock(instance->m_SpinLock);
        return ENOENT;
    }
    struct PathEntry* path_entry = &instance->m_PathEntries[instance->m_PathHashToContent[source_path_ptr].value];
    if (!path_entry->m_Content)
    {
        // Not a file
        Longtail_UnlockSpinLock(instance->m_SpinLock);
        return EINVAL;
    }
    Longtail_Free(path_entry->m_FileName);
    path_entry->m_FileName = 0;
    arrfree(path_entry->m_Content);
    path_entry->m_Content = 0;
    path_entry->m_ParentHash = 0;
    hmdel(instance->m_PathHashToContent, path_hash);
    Longtail_UnlockSpinLock(instance->m_SpinLock);
    return 0;
}

static int InMemStorageAPI_StartFind(struct Longtail_StorageAPI* storage_api, const char* path, Longtail_StorageAPI_HIterator* out_iterator)
{
    struct InMemStorageAPI* instance = (struct InMemStorageAPI*)storage_api;
    Longtail_LockSpinLock(instance->m_SpinLock);
    uint32_t path_hash = path[0] ? InMemStorageAPI_GetPathHash(path) : 0;
    ptrdiff_t* i = (ptrdiff_t*)Longtail_Alloc(sizeof(ptrdiff_t));
    *i = 0;
    while (*i < arrlen(instance->m_PathEntries))
    {
        if (instance->m_PathEntries[*i].m_ParentHash == path_hash)
        {
            *out_iterator = (Longtail_StorageAPI_HIterator)i;
            return 0;
        }
        *i += 1;
    }
    Longtail_Free(i);
    Longtail_UnlockSpinLock(instance->m_SpinLock);
    return ENOENT;
}

static int InMemStorageAPI_FindNext(struct Longtail_StorageAPI* storage_api, Longtail_StorageAPI_HIterator iterator)
{
    struct InMemStorageAPI* instance = (struct InMemStorageAPI*)storage_api;
    ptrdiff_t* i = (ptrdiff_t*)iterator;
    uint32_t path_hash = instance->m_PathEntries[*i].m_ParentHash;
    *i += 1;
    while (*i < arrlen(instance->m_PathEntries))
    {
        if (instance->m_PathEntries[*i].m_ParentHash == path_hash)
        {
            return 0;
        }
        *i += 1;
    }
    return ENOENT;
}
static void InMemStorageAPI_CloseFind(struct Longtail_StorageAPI* storage_api, Longtail_StorageAPI_HIterator iterator)
{
    struct InMemStorageAPI* instance = (struct InMemStorageAPI*)storage_api;
    ptrdiff_t* i = (ptrdiff_t*)iterator;
    Longtail_Free(i);
    Longtail_UnlockSpinLock(instance->m_SpinLock);
}

static const char* InMemStorageAPI_GetFileName(struct Longtail_StorageAPI* storage_api, Longtail_StorageAPI_HIterator iterator)
{
    struct InMemStorageAPI* instance = (struct InMemStorageAPI*)storage_api;
    ptrdiff_t* i = (ptrdiff_t*)iterator;
    if (instance->m_PathEntries[*i].m_Content == 0)
    {
        return 0;
    }
    const char* file_name = instance->m_PathEntries[*i].m_FileName;
    return file_name;
}

static const char* InMemStorageAPI_GetDirectoryName(struct Longtail_StorageAPI* storage_api, Longtail_StorageAPI_HIterator iterator)
{
    struct InMemStorageAPI* instance = (struct InMemStorageAPI*)storage_api;
    uint32_t* i = (uint32_t*)iterator;
    if (instance->m_PathEntries[*i].m_Content != 0)
    {
        return 0;
    }
    return instance->m_PathEntries[*i].m_FileName;
}

static uint64_t InMemStorageAPI_GetEntrySize(struct Longtail_StorageAPI* storage_api, Longtail_StorageAPI_HIterator iterator)
{
    struct InMemStorageAPI* instance = (struct InMemStorageAPI*)storage_api;
    uint32_t* i = (uint32_t*)iterator;
    if (instance->m_PathEntries[*i].m_Content == 0)
    {
        return 0;
    }
    return (uint64_t)arrlen(instance->m_PathEntries[*i].m_Content);
}

static int InMemStorageAPI_Init(struct InMemStorageAPI* storage_api)
{
    storage_api->m_InMemStorageAPI.m_API.Dispose = InMemStorageAPI_Dispose;
    storage_api->m_InMemStorageAPI.OpenReadFile = InMemStorageAPI_OpenReadFile;
    storage_api->m_InMemStorageAPI.GetSize = InMemStorageAPI_GetSize;
    storage_api->m_InMemStorageAPI.Read = InMemStorageAPI_Read;
    storage_api->m_InMemStorageAPI.OpenWriteFile = InMemStorageAPI_OpenWriteFile;
    storage_api->m_InMemStorageAPI.Write = InMemStorageAPI_Write;
    storage_api->m_InMemStorageAPI.SetSize = InMemStorageAPI_SetSize;
    storage_api->m_InMemStorageAPI.CloseFile = InMemStorageAPI_CloseFile;
    storage_api->m_InMemStorageAPI.CreateDir = InMemStorageAPI_CreateDir;
    storage_api->m_InMemStorageAPI.RenameFile = InMemStorageAPI_RenameFile;
    storage_api->m_InMemStorageAPI.ConcatPath = InMemStorageAPI_ConcatPath;
    storage_api->m_InMemStorageAPI.IsDir = InMemStorageAPI_IsDir;
    storage_api->m_InMemStorageAPI.IsFile = InMemStorageAPI_IsFile;
    storage_api->m_InMemStorageAPI.RemoveDir = InMemStorageAPI_RemoveDir;
    storage_api->m_InMemStorageAPI.RemoveFile = InMemStorageAPI_RemoveFile;
    storage_api->m_InMemStorageAPI.StartFind = InMemStorageAPI_StartFind;
    storage_api->m_InMemStorageAPI.FindNext = InMemStorageAPI_FindNext;
    storage_api->m_InMemStorageAPI.CloseFind = InMemStorageAPI_CloseFind;
    storage_api->m_InMemStorageAPI.GetFileName = InMemStorageAPI_GetFileName;
    storage_api->m_InMemStorageAPI.GetDirectoryName = InMemStorageAPI_GetDirectoryName;
    storage_api->m_InMemStorageAPI.GetEntrySize = InMemStorageAPI_GetEntrySize;

    storage_api->m_PathHashToContent = 0;
    storage_api->m_PathEntries = 0;
    return Longtail_CreateSpinLock(&storage_api[1], &storage_api->m_SpinLock);
}

struct Longtail_StorageAPI* Longtail_CreateInMemStorageAPI()
{
    struct InMemStorageAPI* storage_api = (struct InMemStorageAPI*)Longtail_Alloc(sizeof(struct InMemStorageAPI) + Longtail_GetSpinLockSize());
    int err = InMemStorageAPI_Init(storage_api);
    if (err)
    {
        Longtail_Free(storage_api);
        return 0;
    }
    return &storage_api->m_InMemStorageAPI;
}
