#pragma once

#include <string>
#include <vector>

// Cache layout, shared by the thumbnail DLL and the baker.
//
//   %LOCALAPPDATA%\AepThumb\cache\<key>.png     baked frame
//   %LOCALAPPDATA%\AepThumb\queue\<key>.job     pending bake request
//   %LOCALAPPDATA%\AepThumb\aepbake.log         baker log
//
// The key is derived from the project's bytes, not its path. That matters:
// the shell hands a thumbnail provider a stream rather than a filename, and
// keeping the provider on the stream is what lets it stay inside the isolated
// host instead of being loaded into explorer.exe. Hashing content also
// invalidates itself - editing a project changes its key - and lets two copies
// of the same project share one baked frame.

std::wstring AepCacheRoot();                       // ...\AepThumb, created on demand
std::wstring AepCacheDir();                        // ...\AepThumb\cache
std::wstring AepQueueDir();                        // ...\AepThumb\queue
std::wstring AepSpoolDir();                        // ...\AepThumb\spool

// The cache key for a project already held in memory.
std::wstring AepCacheKeyFromContent(const unsigned char* data, size_t size);

// Same, reading the file first. Empty if it cannot be read.
std::wstring AepCacheKey(const std::wstring& projectPath);

// Full path of the cached PNG for a key, or for a project on disk.
std::wstring AepCachedFrameForKey(const std::wstring& key);
std::wstring AepCachedFrame(const std::wstring& projectPath);

// Creates the directory tree. Safe to call repeatedly.
bool AepEnsureCacheDirs();

// Name of the mutex a running baker holds, shared so other processes can tell
// whether one needs starting.
const wchar_t* AepBakerMutexName();
bool AepBakerRunning();

// Drops a bake request in the queue. False when the project is already cached,
// unreadable, or the request is already pending. displayName is only used to
// make the baker's log readable.
bool AepWriteJob(const std::wstring& projectPath,
                 const std::wstring& displayName = std::wstring());

// Reads a queued request back: the project to open, then every file name the
// request was made under. Returns false if the file is unusable.
bool AepReadJob(const std::wstring& jobFile, std::wstring* projectPath,
                std::vector<std::wstring>* names);

// Writes the project's bytes into the spool and queues a request pointing at
// that copy. This is how a thumbnail provider asks for a bake: it is handed a
// stream, never a filename, so a copy is the only thing it can offer the baker.
// False if already cached, already queued, or too large to be worth spooling.
bool AepSpoolAndQueue(const unsigned char* data, size_t size,
                      const std::wstring& displayName);

// True when the path sits inside the spool, i.e. the baker should delete it
// once the frame is cached.
bool AepIsSpooled(const std::wstring& path);

// A bake that failed leaves <key>.fail beside where the frame would be, so the
// provider can show a card instead of asking for the same doomed bake on every
// folder view. Markers expire after a day, which gives transient failures
// another chance.
std::wstring AepFailMarkerForKey(const std::wstring& key);
bool AepBakeFailedRecently(const std::wstring& key);

// Whether any version of After Effects is registered on this machine.
bool AepAfterEffectsInstalled();
