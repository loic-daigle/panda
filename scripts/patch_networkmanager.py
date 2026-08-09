"""
PlatformIO pre-build script: fix an unlocked lwIP call in arduino-esp32.

NetworkManager::hostByName() (Network/src/NetworkManager.cpp) calls the raw
lwIP dns_clear_cache() straight from the calling application task, with no
LOCK_TCPIP_CORE()/UNLOCK_TCPIP_CORE() around it. Raw lwIP core calls (as
opposed to the netconn/socket layer, which locks internally) require the
caller to hold that lock; without it, LWIP_ASSERT_CORE_LOCKED() aborts with
"assert failed: udp_new_ip_type ... Required to lock TCPIP core
functionality!" -- reproducibly, the first time hostByName() clears a still-
outstanding DNS entry (e.g. the first HTTPS fetch right after an NTP sync
that timed out, since HalClock's SNTP client and the fetch's DNS lookup both
touch the DNS table). Wrap the call with the core lock, matching every other
lwIP entry point in the framework.

This targets the global PlatformIO framework package (shared across
projects on this machine), not a per-project libdep, so there's no git
checkout to `git apply` against -- patch the file in place, idempotently.
"""

Import("env")  # noqa: F821
from pathlib import Path

INCLUDE_OLD = '#include "lwip/dns.h"'
INCLUDE_NEW = '#include "lwip/dns.h"\n#include "lwip/tcpip.h"'
CALL_OLD = "    dns_clear_cache();"
CALL_NEW = "    LOCK_TCPIP_CORE();\n    dns_clear_cache();\n    UNLOCK_TCPIP_CORE();"


def patch_network_manager():
    framework_dir = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32"))  # noqa: F821
    target = framework_dir / "libraries" / "Network" / "src" / "NetworkManager.cpp"
    if not target.is_file():
        print(f"NetworkManager.cpp not found at {target}, skipping patch")
        return

    text = target.read_text()
    if CALL_NEW in text:
        return  # already patched

    if CALL_OLD not in text or INCLUDE_OLD not in text:
        raise SystemExit(
            f"NetworkManager.cpp patch target not found in {target} -- "
            "framework layout changed, update scripts/patch_networkmanager.py"
        )

    text = text.replace(INCLUDE_OLD, INCLUDE_NEW, 1)
    text = text.replace(CALL_OLD, CALL_NEW, 1)
    target.write_text(text)
    print(f"Patched NetworkManager.cpp: {target}")


patch_network_manager()
