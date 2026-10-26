/* CTR-NitroFueled-PC: online play without a central server, from guzuold/CTRNativePorted
 * (GPL-2.0, online/). Off unless the launcher sets CTR_ONLINE=1: nothing here runs then and
 * runtime_net.c/runtime_services.c keep their offline contracts.
 *
 * On, ctr_online.dll (online/hle, next to sacredfire.exe) provides the guest's Net, NetCtl,
 * Http, NpManager, NpAuth, NpWebApi, invitation and profile dialog functions, in the guest
 * SysV ABI; they talk only to the player's own online service, which the launcher hosts
 * (online/online_server). Every other import keeps its SacredFire binding, so the renderer,
 * the game loop and everything offline are unchanged.
 *
 * The Demonware client checks every auth3 reply against an RSA-2048 public key stored in the
 * eboot. With CTR_ONLINE_TRAFFIC_KEY naming the online service's own key (DER
 * SubjectPublicKeyInfo, 294 bytes, written by the launcher), the stored key is replaced in
 * memory before the game runs; the verification code itself is unchanged. The original
 * bytes are checked first, so nothing is written to an unknown executable. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>

#define SPKI_LEN 294
/* Demonware auth3 traffic key in the 1.21 image (CTRNativePorted retail_profiles.hpp):
 * EU CUSA14876 and US CUSA13795. Scanned for when neither holds it. */
static const uintptr_t key_sites[] = {0x1becf40, 0x1becf80};
static const unsigned char spki_prefix[] = {0x30, 0x82, 0x01, 0x22, 0x30, 0x0d, 0x06, 0x09, 0x2a,
                                            0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01};
/* SHA-256 of the original key, the only RSA-2048 SubjectPublicKeyInfo in the image. */
static const char original_sha256[] = "356c4bc39a664dd9a14964a3a44258d898a0dbf54cfb10835cbdd63000c9a6b2";

typedef uintptr_t (*ResolveOnline)(const char *nid, const char *library);
static int state; /* 0 unchecked, 1 on, -1 off */
static ResolveOnline resolve_online;

static int sha256_hex(const unsigned char *data, size_t size, char out[65]) {
    typedef LONG (WINAPI *Open)(void **, LPCWSTR, LPCWSTR, ULONG);
    typedef LONG (WINAPI *Hash)(void *, unsigned char *, ULONG, unsigned char *, ULONG, unsigned char *, ULONG);
    typedef LONG (WINAPI *Close)(void *, ULONG);
    HMODULE bcrypt = LoadLibraryW(L"bcrypt.dll");
    Open open = bcrypt ? (Open)(void *)GetProcAddress(bcrypt, "BCryptOpenAlgorithmProvider") : NULL;
    Hash hash = bcrypt ? (Hash)(void *)GetProcAddress(bcrypt, "BCryptHash") : NULL;
    Close close = bcrypt ? (Close)(void *)GetProcAddress(bcrypt, "BCryptCloseAlgorithmProvider") : NULL;
    void *algorithm = NULL;
    unsigned char digest[32];
    if (!open || !hash || !close || open(&algorithm, L"SHA256", NULL, 0) < 0) return 0;
    const LONG status = hash(algorithm, NULL, 0, (unsigned char *)data, (ULONG)size, digest, 32);
    close(algorithm, 0);
    if (status < 0) return 0;
    for (int i = 0; i < 32; ++i) snprintf(out + 2 * i, 3, "%02x", digest[i]);
    return 1;
}

static int original_key_at(const unsigned char *site) {
    char digest[65];
    return !memcmp(site, spki_prefix, sizeof spki_prefix) && sha256_hex(site, SPKI_LEN, digest) &&
           !strcmp(digest, original_sha256);
}

/* Called while the imports are bound: the image is mapped and still writable. */
static void install_traffic_key(void) {
    const char *path = getenv("CTR_ONLINE_TRAFFIC_KEY");
    unsigned char key[SPKI_LEN + 1];
    FILE *file = path && *path ? fopen(path, "rb") : NULL;
    const size_t n = file ? fread(key, 1, sizeof key, file) : 0;
    if (file) fclose(file);
    if (n != SPKI_LEN || memcmp(key, spki_prefix, sizeof spki_prefix)) {
        printf("Online: STOP: CTR_ONLINE_TRAFFIC_KEY is not the online service's RSA-2048 key\n");
        fflush(stdout);
        exit(21);
    }
    uint64_t size = 0;
    unsigned char *image = (unsigned char *)runtime_probe_image(&size);
    unsigned char *site = NULL;
    for (size_t i = 0; i < sizeof key_sites / sizeof *key_sites && !site; ++i)
        if (image && key_sites[i] + SPKI_LEN <= size && original_key_at(image + key_sites[i]))
            site = image + key_sites[i];
    for (uint64_t at = 0; image && !site && at + SPKI_LEN <= size; ++at)
        if (image[at] == 0x30 && original_key_at(image + at)) site = image + at;
    if (!site) {
        printf("Online: STOP: the Demonware key is not in this executable (unsupported eboot)\n");
        fflush(stdout);
        exit(21);
    }
    memcpy(site, key, SPKI_LEN);
    printf("Online: traffic key of the online service installed at image offset 0x%llx\n",
           (unsigned long long)(site - image));
}

static void configure(void) {
    const char *on = getenv("CTR_ONLINE");
    state = -1;
    if (!on || strcmp(on, "1")) return;
    wchar_t dll[MAX_PATH];
    const DWORD length = GetModuleFileNameW(NULL, dll, MAX_PATH);
    wchar_t *slash = length && length < MAX_PATH ? wcsrchr(dll, L'\\') : NULL;
    if (slash && (size_t)(slash - dll) + 16 < MAX_PATH) wcscpy(slash + 1, L"ctr_online.dll");
    HMODULE library = slash ? LoadLibraryW(dll) : NULL;
    if (!library) library = LoadLibraryW(L"ctr_online.dll");
    resolve_online = library ? (ResolveOnline)(void *)GetProcAddress(library, "ctr_hle_resolve_online") : NULL;
    if (!resolve_online) {
        printf("Online: STOP: ctr_online.dll is missing next to sacredfire.exe\n");
        fflush(stdout);
        exit(21);
    }
    install_traffic_key();
    const char *id = getenv("CTR_ONLINE_ID");
    printf("Online: on as %s through this PC's online service\n", id && *id ? id : "CTRPlayer");
    state = 1;
}

/* The invitation (0xa2) and profile (0xb2) dialogs share the screen with the other common
 * dialogs: one at a time (CTRNativePorted main.cpp ctr_sysmodule_load). */
static volatile LONG dialog_owned;
static int dialog_owner(int operation) {
    if (operation == 0) return dialog_owned ? 0 : 1;
    if (operation == 1) return InterlockedCompareExchange(&dialog_owned, 1, 0) == 0;
    if (operation == 2) { InterlockedExchange(&dialog_owned, 0); return 0; }
    return -1;
}
typedef int32_t (ABI *GuestUnload)(uint16_t id);
int32_t runtime_online_module(uint16_t id, int load) {
    if (!state) configure();
    if (state < 0 || (id != 0xa2 && id != 0xb2)) return 0;
    if (load) {
        HMODULE library = GetModuleHandleW(L"ctr_online.dll");
        typedef int (*Load)(int (*)(int));
        Load hook = library ? (Load)(void *)GetProcAddress(library, id == 0xb2 ? "ctr_hle_profile_load"
                                                                                 : "ctr_hle_invitation_load") : NULL;
        return hook ? hook(dialog_owner) : 0;
    }
    /* Only the invitation dialog has an unload lifecycle in ctr_online.dll. */
    GuestUnload unload = id == 0xa2 ? (GuestUnload)resolve_online("eR2bZFAAU0Q", "libSceSysmodule") : NULL;
    return unload ? unload(id) : 0;
}

uintptr_t runtime_online_resolve(const char *name) {
    if (!state) configure();
    if (state < 0) return 0;
    /* NID#library[#module]: ctr_online.dll is keyed by NID and library. */
    const char *hash = strchr(name, '#');
    if (!hash || hash - name != 11) return 0;
    /* sceSysmoduleUnloadModule stays in runtime_content.c, which forwards 0xa2 here. */
    if (!strncmp(name, "eR2bZFAAU0Q", 11)) return 0;
    char nid[12], library[96];
    memcpy(nid, name, 11);
    nid[11] = 0;
    const char *end = strchr(hash + 1, '#');
    const size_t n = end ? (size_t)(end - hash - 1) : strlen(hash + 1);
    if (!n || n >= sizeof library) return 0;
    memcpy(library, hash + 1, n);
    library[n] = 0;
    return resolve_online(nid, library);
}
#else
uintptr_t runtime_online_resolve(const char *name) { (void)name; return 0; }
int32_t runtime_online_module(uint16_t id, int load) { (void)id; (void)load; return 0; }
#endif
