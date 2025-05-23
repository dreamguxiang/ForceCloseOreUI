

#include "api/memory/Hook.h"
#include "api/memory/android/Memory.h"

#include <cstdint>
#include <string>
#include <vector>

// Define RT_SUCCESS if not already defined
#ifndef RT_SUCCESS
#define RT_SUCCESS 0
#endif

#include <android/log.h>

#define LOGI(...)                                                              \
  __android_log_print(ANDROID_LOG_INFO, "LeviLogger", __VA_ARGS__)

#include <mutex>
#include <set>
#include <type_traits>
#include <unordered_map>

#include <dlfcn.h>

typedef enum {
  SHADOWHOOK_MODE_SHARED = 0, // a function can be hooked multiple times
  SHADOWHOOK_MODE_UNIQUE = 1  // a function can only be hooked once, and hooking
                              // again will report an error
} shadowhook_mode_t;

static int (*shadowhook_init)(shadowhook_mode_t mode, bool debuggable);
static void *(*shadowhook_hook_func_addr)(void *func_addr, void *new_addr,
                                          void **orig_addr);
static int (*shadowhook_unhook)(void *stub);
namespace memory {

struct HookElement {
  FuncPtr detour{};
  FuncPtr *originalFunc{};
  HookPriority priority{};
  int id{};

  bool operator<(const HookElement &other) const {
    if (priority != other.priority)
      return priority < other.priority;
    return id < other.id;
  }
};

struct HookData {
  FuncPtr target{};
  FuncPtr origin{};
  FuncPtr start{};
  FuncPtr stub{};
  int hookId{};
  std::set<HookElement> hooks{};

  void updateCallList() {
    FuncPtr *last = nullptr;
    for (auto &item : this->hooks) {
      if (last == nullptr) {
        this->start = item.detour;
        last = item.originalFunc;
        *last = this->origin;
      } else {
        *last = item.detour;
        last = item.originalFunc;
      }
    }

    if (last == nullptr) {
      this->start = this->origin;
    } else {
      *last = this->origin;
    }
  }

  int incrementHookId() { return ++hookId; }
};

std::unordered_map<FuncPtr, std::shared_ptr<HookData>> &getHooks() {
  static std::unordered_map<FuncPtr, std::shared_ptr<HookData>> hooks;
  return hooks;
}

static std::mutex hooksMutex{};

int hook(FuncPtr target, FuncPtr detour, FuncPtr *originalFunc,
         HookPriority priority, bool suspendThreads) {
  std::lock_guard lock(hooksMutex);

  static bool init = false;

  if (!init) {
    void *handle = dlopen("libshadowhook.so", RTLD_LAZY);
    shadowhook_init =
        (int (*)(shadowhook_mode_t, bool))(dlsym(handle, "shadowhook_init"));

    shadowhook_hook_func_addr = (void *(*)(void *, void *, void **))(
        dlsym(handle, "shadowhook_hook_func_addr"));

    shadowhook_unhook =
        (int (*)(void *func_addr))(dlsym(handle, "shadowhook_unhook"));

    auto result = shadowhook_init(SHADOWHOOK_MODE_SHARED, false);
    if (result == 0) {
      init = true;
    }
  }

  LOGI("target = 0x%lx, detour = 0x%lx", target, detour);

  auto it = getHooks().find(target);
  if (it != getHooks().end()) {
    auto hookData = it->second;
    hookData->hooks.insert(
        {detour, originalFunc, priority, hookData->incrementHookId()});
    hookData->updateCallList();

    shadowhook_hook_func_addr(target, hookData->start, &hookData->origin);
    return 0;
  }

  auto hookData = std::make_shared<HookData>();
  hookData->target = target;
  hookData->origin = nullptr;
  hookData->start = detour;
  hookData->stub = shadowhook_hook_func_addr(target, detour, &hookData->origin);
  hookData->hooks.insert(
      {detour, originalFunc, priority, hookData->incrementHookId()});
  if (!hookData->stub) {
    return -1;
  }

  hookData->updateCallList();
  getHooks().emplace(target, hookData);
  return 0;
}

bool unhook(FuncPtr target, FuncPtr detour, bool suspendThreads) {
  std::lock_guard lock(hooksMutex);

  if (!target)
    return false;

  auto hookDataIter = getHooks().find(target);
  if (hookDataIter == getHooks().end())
    return false;

  auto &hookData = hookDataIter->second;

  for (auto it = hookData->hooks.begin(); it != hookData->hooks.end(); ++it) {
    if (it->detour == detour) {
      hookData->hooks.erase(it);
      hookData->updateCallList();

      if (hookData->hooks.empty()) {
        shadowhook_unhook(hookData->stub);
        getHooks().erase(target);
      } else {
        shadowhook_hook_func_addr(target, hookData->start, &hookData->origin);
      }

      return true;
    }
  }

  return false;
}

void unhookAll() {
  std::lock_guard lock(hooksMutex);

  for (auto &[target, hookData] : getHooks()) {
    shadowhook_unhook(hookData->stub);
  }

  getHooks().clear();
}

uintptr_t getLibBase(const char *libName) {
  FILE *fp = fopen("/proc/self/maps", "r");
  if (!fp)
    return 0;

  uintptr_t base = 0;
  char line[512];

  while (fgets(line, sizeof(line), fp)) {
    if (strstr(line, libName)) {
      uintptr_t temp;
      if (sscanf(line, "%lx-%*lx", &temp) == 1) {
        base = temp;
        break;
      }
    }
  }

  fclose(fp);
  return base;
}

size_t getLibSize(const char *libName) {
  FILE *fp = fopen("/proc/self/maps", "r");
  if (!fp)
    return 0;

  size_t totalSize = 0;
  char line[512];

  while (fgets(line, sizeof(line), fp)) {
    if (strstr(line, libName)) {
      uintptr_t start = 0, end = 0;
      if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
        totalSize += (end - start);
      }
    }
  }

  fclose(fp);
  return totalSize;
}

FuncPtr resolveIdentifier(const char *identifier) {
  static bool initialized = false;
  static uintptr_t base = 0;
  static size_t size = 0;

  if (!initialized) {
    base = getLibBase("libminecraftpe.so");
    size = getLibSize("libminecraftpe.so");

    LOGI("libminecraftpe base = 0x%lx, size = 0x%zx", base, size);

    initialized = true;
  }

  if (base == 0 || size == 0) {
    LOGI("Failed to find libminecraftpe.so");
    return nullptr;
  }

  uintptr_t result = resolveSignature(base, size, identifier);
  if (result) {
    LOGI("[resolveIdentifier] Resolved identifier [%s] to address 0x%lx",
         identifier, result);
    return reinterpret_cast<FuncPtr>(result);
  } else {
    LOGI("[resolveIdentifier] Failed to resolve signature for [%s]",
         identifier);
    return nullptr;
  }
}

FuncPtr resolveIdentifier(std::initializer_list<const char *> identifiers) {
  for (const auto &identifier : identifiers) {
    FuncPtr result = resolveIdentifier(identifier);
    if (result != nullptr) {
      return result;
    }
  }
  return nullptr;
}

} // namespace memory