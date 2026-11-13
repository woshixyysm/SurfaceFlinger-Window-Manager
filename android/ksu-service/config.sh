#!/system/bin/sh
# Runtime configuration for sfwm-service. Edit and run `sfwm-service restart`.
#
# Nothing here is stored under /data/data/com.termux: PackageManagerService has wiped that
# directory on this device more than once, and the service must survive it.
# The runtime lives in /dev/sfwm, which is tmpfs: not under /data, not a mount point, so it
# never appears in mountinfo and no data-directory cleanup can reach it.

# --- where things are -------------------------------------------------------------------
SFWM_BIN="${SFWM_BIN:-/data/data/com.termux/files/home/lg/sfwm/sfwm}"
SFWM_PREFIX="${SFWM_PREFIX:-/data/data/com.termux/files/usr}"
SFWM_RUN="${SFWM_RUN:-/dev/sfwm}"

# Owner of the socket and of the process. mc runs as the Termux uid and has to connect.
SFWM_UID="${SFWM_UID:-10391}"

# --- how it runs ------------------------------------------------------------------------
SFWM_SCALE="${SFWM_SCALE:-1}"
SFWM_FPS="${SFWM_FPS:-165}"
SFWM_ASYNC="${SFWM_ASYNC:-1}"
SFWM_ZC="${SFWM_ZC:-1}"
SFWM_ARGS="${SFWM_ARGS:---z=1000000 --grab}"

# Idle exit is off by default: with the module enabled the intent is a service that is always
# there. Set to a number of seconds to have it stop itself when no client has been connected
# for that long, which is what you want if the background cost ever matters.
SFWM_IDLE_EXIT="${SFWM_IDLE_EXIT:-0}"

# --- cursor -----------------------------------------------------------------------------
# 1 shows the compositor cursor, 0 hides it entirely.
SFWM_CURSOR="${SFWM_CURSOR:-1}"
# Empty means "whatever the client asks for", which is what a themed desktop expects.
XCURSOR_THEME="${XCURSOR_THEME:-}"
XCURSOR_SIZE="${XCURSOR_SIZE:-32}"
XCURSOR_PATH="${XCURSOR_PATH:-$SFWM_PREFIX/share/icons}"

# --- logging ----------------------------------------------------------------------------
# Small and rotated, and deliberately not in /data/local/tmp.
SFWM_LOG="${SFWM_LOG:-$SFWM_RUN/sfwm.log}"
SFWM_LOG_MAX="${SFWM_LOG_MAX:-262144}"
