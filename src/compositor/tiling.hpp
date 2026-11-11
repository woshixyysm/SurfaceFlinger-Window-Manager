// SPDX-License-Identifier: AGPL-3.0-or-later
// tiling.hpp - tiling window management for the tiled workspace.
//
// The model follows Hyprland's user-facing behaviour: two layouts (dwindle and master),
// directional focus and swap, a ratio that can be dragged, gaps, per-workspace order and a
// floating exception for windows that should not take part.
//
// The composer never resizes a surface itself. A tile is applied by moving the layer and
// asking the client for the tile's logical size with a configure, which is the same path
// the drag-resize gesture already uses; the window becomes exactly the tile when the next
// buffer arrives.
//
// Depends on include order: View (view.hpp), the output and workspace state (config.hpp,
// workspace.hpp), and the forward declarations below for functions defined later.
#pragma once

static void    kb_set_focus(View *v);
static int32_t client_scale(View *v);
static void    toplevel_send_configure(View *v);

// ------------------------------------------------------------------ state
enum TileLayout { TILE_DWINDLE = 0, TILE_MASTER = 1 };

static bool    g_tile_on = false;
static TileLayout g_tile_layout = TILE_DWINDLE;
static int32_t g_tile_gap = 12;        // outer and inner gap, device pixels
static float   g_tile_ratio = 0.5f;    // master column share
static int     g_tile_masters = 1;     // windows in the master column

// Insertion order of the tiled windows, and the ones that opted out. Append-only order is
// what dwindle needs; a window removed from the tree simply disappears from the list, so
// no explicit tree surgery is required when a client goes away.
static std::vector<View *> g_tile_order;
static std::vector<View *> g_tile_floating;
static View *g_tile_focus = nullptr;

// A dwindle split belongs to the window that opens its group. Storing the ratio per split
// is what keeps a resize local: moving one boundary cannot move the rest of the screen.
// The owner is opaque: a View for a Wayland window, an AppWin for an Android one.
struct TileSplit { void *owner; float ratio; };
static std::vector<TileSplit> g_tile_splits;

// Written on every layout pass, so a resize can turn a pixel delta into a ratio delta.
struct TileNode { void *owner; sf::Rect rect; bool vertical; float ratio; };
static std::vector<TileNode> g_tile_nodes;

// ------------------------------------------------------------------ helpers
static int tile_index_of(const std::vector<View *> &list, View *v);   // defined below

static bool tile_known(View *v) {
    if (!v) return false;
    for (View *o : g_srv.views) if (o == v) return true;
    return false;
}

static bool tile_candidate(View *v) {
    if (!tile_known(v) || !v->mapped) return false;
    return !v->is_subsurface && !v->is_cursor && !v->is_layer && !v->is_x_root &&
           !v->is_ime && !v->is_popup && !v->is_lock;
}

static bool tile_is_floating(View *v) {
    for (View *f : g_tile_floating) if (f == v) return true;
    return false;
}

// True while the window's geometry belongs to the layout. Gestures and commands ask this
// before they touch a window, so a managed window can never be dragged out of its tile.
static bool tile_owns(View *v) {
    if (!g_tile_on || !v) return false;
    if (!tile_candidate(v) || tile_is_floating(v)) return false;
    return v->ws == g_ws_active && tile_index_of(g_tile_order, v) >= 0;
}

static float tile_split_of(void *owner) {
    for (const TileSplit &s : g_tile_splits) if (s.owner == owner) return s.ratio;
    return 0.f;
}

static void tile_split_set(void *owner, float ratio) {
    if (ratio < 0.1f) ratio = 0.1f;
    if (ratio > 0.9f) ratio = 0.9f;
    for (TileSplit &s : g_tile_splits) {
        if (s.owner != owner) continue;
        s.ratio = ratio;
        return;
    }
    g_tile_splits.push_back(TileSplit{owner, ratio});
}

static const TileNode *tile_node_of(void *owner) {
    for (const TileNode &n : g_tile_nodes) if (n.owner == owner) return &n;
    return nullptr;
}

// A split owner is a View or an AppWin, so liveness is checked for both kinds.
static bool tile_owner_alive(void *owner) {
    for (AppWin *a : g_apps)
        if ((void *)a == owner) return a->w > 0 && a->h > 0;
    return tile_candidate((View *)owner);
}

static void tile_untrack(View *v) {
    for (size_t i = 0; i < g_tile_order.size(); i++) {
        if (g_tile_order[i] != v) continue;
        g_tile_order.erase(g_tile_order.begin() + i);
        return;
    }
}

// Drops destroyed, unmapped and floating windows. Membership is checked before any field
// is read, so a pointer to a view that has already been deleted is never dereferenced.
static void tile_prune() {
    std::vector<View *> kept;
    for (View *v : g_tile_order) {
        if (!tile_candidate(v)) continue;
        if (tile_is_floating(v)) continue;
        kept.push_back(v);
    }
    g_tile_order.swap(kept);
    for (size_t i = 0; i < g_tile_splits.size(); ) {
        if (tile_owner_alive(g_tile_splits[i].owner)) i++;
        else g_tile_splits.erase(g_tile_splits.begin() + i);
    }
    if (g_tile_focus && (!tile_candidate(g_tile_focus) || g_tile_focus->ws != g_ws_active))
        g_tile_focus = nullptr;
}

static void tile_active_list(std::vector<View *> &out) {
    out.clear();
    for (View *v : g_tile_order)
        if (v->ws == g_ws_active) out.push_back(v);
}

static int tile_index_of(const std::vector<View *> &list, View *v) {
    for (size_t i = 0; i < list.size(); i++) if (list[i] == v) return (int)i;
    return -1;
}

static sf::Rect tile_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    sf::Rect r;
    r.left = x;
    r.top = y;
    r.right = x + (w > 0 ? w : 1);
    r.bottom = y + (h > 0 ? h : 1);
    return r;
}

// The windows the layout places, in order: the Wayland views first, then the Android
// windows. The keys are opaque so one layout pass can size both kinds.
static void tile_keys(const std::vector<View *> &views, std::vector<void *> &keys) {
    keys.clear();
    for (View *v : views) keys.push_back(v);
    for (AppWin *a : g_apps)
        if (a->ws == g_ws_active && a->w > 0 && a->h > 0) keys.push_back(a);
}

// ------------------------------------------------------------------ layout
// Geometry for the active list. Both layouts work on the same ordered list, so focus,
// swap and workspace changes never have to reason about a tree.
static void tile_rects(const std::vector<void *> &keys, std::vector<sf::Rect> &out) {
    out.clear();
    const int n = (int)keys.size();
    if (n <= 0) return;

    int32_t gap = g_tile_gap;
    if (gap < 0) gap = 0;
    int32_t x0 = gap, y0 = gap;
    int32_t w0 = g_out_w - 2 * gap;
    int32_t h0 = g_out_h - 2 * gap;
    if (w0 < n) w0 = n;
    if (h0 < n) h0 = n;

    if (g_tile_layout == TILE_MASTER) {
        int masters = g_tile_masters < 1 ? 1 : g_tile_masters;
        if (masters > n) masters = n;
        float ratio = g_tile_ratio;
        if (ratio < 0.2f) ratio = 0.2f;
        if (ratio > 0.85f) ratio = 0.85f;
        int32_t mw = (int32_t)(w0 * ratio + 0.5f);
        if (mw < 1) mw = 1;
        if (mw > w0 - 1) mw = w0 - 1;
        int32_t sx = x0 + mw + gap;
        int32_t sw = w0 - mw - gap;
        if (sw < 1) sw = 1;

        for (int i = 0; i < masters; i++) {
            int32_t h = (h0 - gap * (masters - 1)) / masters;
            if (h < 1) h = 1;
            if (i == masters - 1) h = h0 - (h + gap) * (masters - 1);
            int32_t y = y0 + i * (h + gap);
            out.push_back(tile_rect(x0, y, mw, h));
        }
        const int stack = n - masters;
        for (int i = 0; i < stack; i++) {
            int32_t h = (h0 - gap * (stack - 1)) / stack;
            if (h < 1) h = 1;
            if (i == stack - 1) h = h0 - (h + gap) * (stack - 1);
            int32_t y = y0 + i * (h + gap);
            out.push_back(tile_rect(sx, y, sw, h));
        }
        return;
    }

    // Dwindle: every window but the last splits the remaining rectangle, always along its
    // longer side, so tiles stay as close to square as the count allows.
    g_tile_nodes.clear();
    int32_t x = x0, y = y0, w = w0, h = h0;
    for (int i = 0; i < n; i++) {
        if (i == n - 1) {
            out.push_back(tile_rect(x, y, w, h));
            break;
        }
        const int remaining = n - i;
        const bool vertical = h > w;
        // No stored ratio means the plain even split, so the default layout is unchanged.
        const float stored = tile_split_of(keys[i]);
        const float even = 1.f / (float)remaining;
        float ratio = stored > 0.f ? stored : even;
        if (ratio < 0.1f) ratio = 0.1f;
        if (ratio > 0.9f) ratio = 0.9f;
        g_tile_nodes.push_back(TileNode{keys[i], tile_rect(x, y, w, h), vertical, ratio});
        if (vertical) {
            int32_t th = stored > 0.f ? (int32_t)((float)h * ratio + 0.5f) : h / remaining;
            if (th < 1) th = 1;
            if (th > h - 1) th = h - 1;
            out.push_back(tile_rect(x, y, w, th));
            y += th + gap;
            h -= th + gap;
            if (h < 1) h = 1;
        } else {
            int32_t tw = stored > 0.f ? (int32_t)((float)w * ratio + 0.5f) : w / remaining;
            if (tw < 1) tw = 1;
            if (tw > w - 1) tw = w - 1;
            out.push_back(tile_rect(x, y, tw, h));
            x += tw + gap;
            w -= tw + gap;
            if (w < 1) w = 1;
        }
    }
}

static void tile_apply() {
    if (!g_tile_on) return;
    std::vector<View *> list;
    std::vector<void *> keys;
    std::vector<sf::Rect> rects;
    tile_active_list(list);
    tile_keys(list, keys);
    tile_rects(keys, rects);
    if (keys.size() != rects.size()) return;

    sf::begin_batch();
    for (size_t i = 0; i < list.size(); i++) {
        View *v = list[i];
        const sf::Rect &r = rects[i];
        int32_t w = r.right - r.left;
        int32_t h = r.bottom - r.top;
        int32_t sc = client_scale(v);
        if (sc < 1) sc = 1;
        v->x = r.left;
        v->y = r.top;
        v->req_w = w / sc;
        v->req_h = h / sc;
        if (v->req_w < 1) v->req_w = 1;
        if (v->req_h < 1) v->req_h = 1;
        v->place();
        toplevel_send_configure(v);
    }
    sf::end_batch();

    // An Android window has no surface to configure: its display is hosted by the module,
    // so the tile is applied by moving that window.
    for (size_t i = list.size(); i < keys.size(); i++) {
        const sf::Rect &r = rects[i];
        appwin_place((AppWin *)keys[i], r.left, r.top, r.right - r.left, r.bottom - r.top);
    }
}

// Publishes the current tiling order to the layout. Safe to call from any event: it prunes
// first, so a destroyed window can never reach the geometry code.
static void tile_sync() {
    if (!g_tile_on) return;
    tile_prune();
    for (View *v : g_srv.views) {
        if (!tile_candidate(v)) continue;
        if (v->ws != g_ws_active) continue;
        if (tile_is_floating(v)) continue;
        if (tile_index_of(g_tile_order, v) < 0) g_tile_order.push_back(v);
    }
    tile_apply();
}

// ------------------------------------------------------------------ focus and swap
static bool tile_current_rects(std::vector<View *> &list, std::vector<sf::Rect> &rects) {
    tile_prune();
    tile_active_list(list);
    std::vector<void *> keys;
    tile_keys(list, keys);
    tile_rects(keys, rects);
    return !list.empty() && list.size() == keys.size() && rects.size() == keys.size();
}

// Picks the window whose centre is nearest in the requested direction. Comparing centres
// and weighting the cross axis is what makes the choice feel like a grid instead of a list.
static View *tile_neighbour(const std::vector<View *> &list, const std::vector<sf::Rect> &rects,
                            View *from, int dx, int dy) {
    const int fi = tile_index_of(list, from);
    if (fi < 0) return nullptr;
    const sf::Rect &fr = rects[fi];
    const long fcx = (long)fr.left + fr.right;
    const long fcy = (long)fr.top + fr.bottom;

    View *best = nullptr;
    long best_score = 0;
    View *fallback = nullptr;
    long fallback_score = 0;
    for (size_t i = 0; i < list.size(); i++) {
        if ((int)i == fi) continue;
        const sf::Rect &r = rects[i];
        const long ax = ((long)r.left + r.right) - fcx;
        const long ay = ((long)r.top + r.bottom) - fcy;
        const long adx = ax < 0 ? -ax : ax;
        const long ady = ay < 0 ? -ay : ay;
        if (dx != 0) {
            if ((dx > 0 && ax <= 0) || (dx < 0 && ax >= 0)) continue;
        } else if (dy != 0) {
            if ((dy > 0 && ay <= 0) || (dy < 0 && ay >= 0)) continue;
        } else {
            continue;
        }
        const long primary = dx != 0 ? adx : ady;
        const long cross = dx != 0 ? ady : adx;
        const long score = primary * 4 + cross;
        if (!best || score < best_score) {
            best = list[i];
            best_score = score;
        }
        // The opposite direction still gives a sensible wrap-around target.
        if (!fallback || primary + cross < fallback_score) {
            fallback = list[i];
            fallback_score = primary + cross;
        }
    }
    return best ? best : fallback;
}

static void tile_raise(View *v) {
    if (!v || !v->layer.win) return;
    int32_t top = -1;
    for (View *o : g_srv.views)
        if (o->mapped && !o->is_subsurface && o->z > top) top = o->z;
    if (v->z <= top) {
        v->z = top + 1;
        v->place();
    }
}

static View *tile_focused(const std::vector<View *> &list) {
    if (g_tile_focus && tile_index_of(list, g_tile_focus) >= 0) return g_tile_focus;
    if (g_srv.kb_focus && tile_index_of(list, g_srv.kb_focus) >= 0) return g_srv.kb_focus;
    return list.empty() ? nullptr : list.front();
}

static void tile_focus_dir(int dx, int dy) {
    std::vector<View *> list;
    std::vector<sf::Rect> rects;
    if (!tile_current_rects(list, rects)) return;
    View *from = tile_focused(list);
    if (!from) return;
    View *next = tile_neighbour(list, rects, from, dx, dy);
    if (!next) return;
    g_tile_focus = next;
    kb_set_focus(next);
    tile_raise(next);
    printf("[tile] focus '%s'\n", next->title[0] ? next->title : "view");
}

static void tile_swap_dir(int dx, int dy) {
    std::vector<View *> list;
    std::vector<sf::Rect> rects;
    if (!tile_current_rects(list, rects)) return;
    View *from = tile_focused(list);
    if (!from) return;
    View *other = tile_neighbour(list, rects, from, dx, dy);
    if (!other) return;
    int ia = tile_index_of(g_tile_order, from);
    int ib = tile_index_of(g_tile_order, other);
    if (ia < 0 || ib < 0) return;
    View *tmp = g_tile_order[ia];
    g_tile_order[ia] = g_tile_order[ib];
    g_tile_order[ib] = tmp;
    g_tile_focus = other;
    kb_set_focus(other);
    tile_sync();
    printf("[tile] swap '%s' <-> '%s'\n",
           from->title[0] ? from->title : "view", other->title[0] ? other->title : "view");
}

// ------------------------------------------------------------------ resize
// Hyprland moves a boundary instead of re-laying the screen. The split the focused window
// shares with its neighbour in the requested direction changes; no other window moves, so
// nothing grows to fill a gap behind the user's back.
static bool tile_resize_boundary(View *a, View *b, float delta) {
    const int ia = tile_index_of(g_tile_order, a);
    const int ib = tile_index_of(g_tile_order, b);
    if (ia < 0 || ib < 0) return false;
    View *owner = ia < ib ? a : b;          // the window that opens the group owns the split
    const TileNode *node = tile_node_of(owner);
    if (!node) return false;
    const float before = node->ratio;
    // The first child holds the head of the node, so it grows with the ratio; the second
    // child grows when the ratio falls.
    const float next = before + (owner == a ? delta : -delta);
    tile_split_set(owner, next);
    tile_sync();
    printf("[tile] split '%s' %.2f -> %.2f\n", owner->title[0] ? owner->title : "view",
           before, tile_split_of(owner));
    return true;
}

static bool tile_resize_dir(int dx, int dy, float delta) {
    std::vector<View *> list;
    std::vector<sf::Rect> rects;
    if (!tile_current_rects(list, rects)) return false;
    View *v = tile_focused(list);
    if (!v) return false;
    if (g_tile_layout == TILE_MASTER) {
        // The master column is a single global boundary; there is nothing local about it.
        g_tile_ratio += (float)(dx + dy) * delta;
        if (g_tile_ratio < 0.2f) g_tile_ratio = 0.2f;
        if (g_tile_ratio > 0.85f) g_tile_ratio = 0.85f;
        printf("[tile] ratio %.2f\n", g_tile_ratio);
        tile_sync();
        return true;
    }
    View *other = tile_neighbour(list, rects, v, dx, dy);
    if (!other) {
        printf("[tile] nothing to resize against on that side\n");
        return false;
    }
    return tile_resize_boundary(v, other, delta);
}

// A pinch on a managed window drags its boundary. Pixels are turned into a ratio through
// the node's own extent, so the same finger travel means the same thing at any size.
static bool tile_resize_pixels(int32_t px) {
    std::vector<View *> list;
    std::vector<sf::Rect> rects;
    if (!tile_current_rects(list, rects)) return false;
    View *v = tile_focused(list);
    if (!v) return false;
    const TileNode *node = tile_node_of(v);
    if (!node) {
        const int i = tile_index_of(list, v);
        if (i <= 0) return false;
        node = tile_node_of(list[i - 1]);
        if (!node) return false;
    }
    const int32_t extent = node->vertical ? (node->rect.bottom - node->rect.top)
                                          : (node->rect.right - node->rect.left);
    if (extent < 1) return false;
    View *owner = (View *)node->owner;
    float delta = (float)px / (float)extent;
    if (owner != v) delta = -delta;
    tile_split_set(owner, node->ratio + delta);
    tile_apply();
    return true;
}

// ------------------------------------------------------------------ commands
static void tile_toggle() {
    g_tile_on = !g_tile_on;
    printf("[tile] %s (layout=%s gap=%d)\n", g_tile_on ? "on" : "off",
           g_tile_layout == TILE_MASTER ? "master" : "dwindle", (int)g_tile_gap);
    if (g_tile_on) {
        tile_sync();
        return;
    }
    // Leaving the tile mode must not leave the windows looking like tiles: the forced
    // logical size is cleared so the next configure no longer asks for it, and every
    // window keeps its current position and size as a normal floating window.
    for (View *v : g_tile_order) {
        if (!tile_candidate(v)) continue;
        v->req_w = 0;
        v->req_h = 0;
        v->place();
    }
    g_tile_order.clear();
    g_tile_floating.clear();
    g_tile_splits.clear();
    g_tile_nodes.clear();
    g_tile_focus = nullptr;
}

static void tile_set_layout(const char *name) {
    if (name && !strcmp(name, "master")) g_tile_layout = TILE_MASTER;
    else g_tile_layout = TILE_DWINDLE;
    printf("[tile] layout %s\n", g_tile_layout == TILE_MASTER ? "master" : "dwindle");
    tile_sync();
}

static void tile_toggle_float() {
    std::vector<View *> list;
    std::vector<sf::Rect> rects;
    if (!tile_current_rects(list, rects)) return;
    View *v = tile_focused(list);
    if (!v) return;

    if (tile_is_floating(v)) {
        for (size_t i = 0; i < g_tile_floating.size(); i++) {
            if (g_tile_floating[i] != v) continue;
            g_tile_floating.erase(g_tile_floating.begin() + i);
            break;
        }
        g_tile_order.push_back(v);
        printf("[tile] '%s' returns to the layout\n", v->title[0] ? v->title : "view");
    } else {
        g_tile_floating.push_back(v);
        tile_untrack(v);
        printf("[tile] '%s' is floating\n", v->title[0] ? v->title : "view");
    }
    tile_sync();
}

static void tile_toggle_fullscreen() {
    std::vector<View *> list;
    std::vector<sf::Rect> rects;
    if (!tile_current_rects(list, rects)) return;
    View *v = tile_focused(list);
    if (!v) return;
    v->want_fullscreen = !v->want_fullscreen;
    toplevel_send_configure(v);
    printf("[tile] fullscreen %s for '%s'\n", v->want_fullscreen ? "on" : "off",
           v->title[0] ? v->title : "view");
}

static int tile_dir_of(const char *s, int *dx, int *dy) {
    *dx = 0;
    *dy = 0;
    if (!s) return 0;
    if (!strcmp(s, "l") || !strcmp(s, "left")) { *dx = -1; return 1; }
    if (!strcmp(s, "r") || !strcmp(s, "right")) { *dx = 1; return 1; }
    if (!strcmp(s, "u") || !strcmp(s, "up")) { *dy = -1; return 1; }
    if (!strcmp(s, "d") || !strcmp(s, "down")) { *dy = 1; return 1; }
    if (!strcmp(s, "next")) { *dx = 1; return 1; }
    if (!strcmp(s, "prev")) { *dx = -1; return 1; }
    return 0;
}

static void tile_on_focus(View *v) {
    if (!g_tile_on || !v) return;
    if (!tile_candidate(v) || tile_is_floating(v)) return;
    g_tile_focus = v;
}

// Three-finger gesture: horizontal moves the focus, vertical swaps, a tap toggles float.
static void tile_swipe_end(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    const int32_t threshold = 40 * (g_scale > 0 ? g_scale : 1);
    const int32_t dx = x1 - x0;
    const int32_t dy = y1 - y0;
    const int32_t adx = dx < 0 ? -dx : dx;
    const int32_t ady = dy < 0 ? -dy : dy;
    if (adx < threshold && ady < threshold) {
        tile_toggle_float();
        return;
    }
    if (adx >= ady) tile_focus_dir(dx > 0 ? 1 : -1, 0);
    else tile_swap_dir(0, dy > 0 ? 1 : -1);
}

static void tile_command(const std::string &args) {
    char verb[32] = {0}, a[32] = {0};
    float f = 0.f;
    int n = 0;
    if (sscanf(args.c_str(), "%31s %31s %f", verb, a, &f) >= 1) n = 1;
    // A one-token argument ("tile gap 20") leaves the float untouched, so the numeric text
    // in the second token is the fallback. Without this every value command read as zero.
    float value = f;
    if (value == 0.f && a[0]) value = (float)atof(a);

    if (!n) { tile_toggle(); return; }

    if (!strcmp(verb, "on")) { g_tile_on = true; tile_sync(); printf("[tile] on\n"); return; }
    if (!strcmp(verb, "off")) { tile_toggle(); return; }
    if (!strcmp(verb, "toggle")) { tile_toggle(); return; }
    if (!strcmp(verb, "layout")) { tile_set_layout(a); return; }
    if (!strcmp(verb, "sync")) { tile_sync(); printf("[tile] synced %d window(s)\n", (int)g_tile_order.size()); return; }
    if (!strcmp(verb, "float")) { tile_toggle_float(); return; }
    if (!strcmp(verb, "fullscreen") || !strcmp(verb, "full")) { tile_toggle_fullscreen(); return; }
    if (!strcmp(verb, "ratio")) {
        if (value <= 0.f) value = 0.5f;
        if (value < 0.2f) value = 0.2f;
        if (value > 0.85f) value = 0.85f;
        g_tile_ratio = value;
        printf("[tile] ratio %.2f\n", g_tile_ratio);
        tile_sync();
        return;
    }
    if (!strcmp(verb, "master")) {
        if (value >= 1.f) g_tile_masters = (int)value;
        if (g_tile_masters < 1) g_tile_masters = 1;
        printf("[tile] masters %d\n", g_tile_masters);
        tile_sync();
        return;
    }
    if (!strcmp(verb, "gap")) {
        if (value >= 0.f) g_tile_gap = (int32_t)value;
        printf("[tile] gap %d\n", (int)g_tile_gap);
        tile_sync();
        return;
    }
    if (!strcmp(verb, "focus") || !strcmp(verb, "movefocus")) {
        int dx = 0, dy = 0;
        if (tile_dir_of(a, &dx, &dy)) tile_focus_dir(dx, dy);
        return;
    }
    if (!strcmp(verb, "swap") || !strcmp(verb, "swapwindow")) {
        int dx = 0, dy = 0;
        if (tile_dir_of(a, &dx, &dy)) tile_swap_dir(dx, dy);
        return;
    }
    if (!strcmp(verb, "resize")) {
        int dx = 0, dy = 0;
        if (!tile_dir_of(a, &dx, &dy)) return;
        float delta = f != 0.f ? f : 0.05f;
        if (delta < -0.5f) delta = -0.5f;
        if (delta > 0.5f) delta = 0.5f;
        tile_resize_dir(dx, dy, delta);
        return;
    }
    if (!strcmp(verb, "even")) {
        // Forgets every custom split, so the layout returns to equal shares.
        g_tile_splits.clear();
        printf("[tile] splits reset (%d window(s))\n", (int)g_tile_order.size());
        tile_sync();
        return;
    }
    printf("[tile] usage: tile on|off|toggle|sync|float|fullscreen|layout dwindle|master|"
           "focus l|r|u|d|swap l|r|u|d|resize l|r|u|d [ratio]|ratio f|master n|gap px|even\n");
}
