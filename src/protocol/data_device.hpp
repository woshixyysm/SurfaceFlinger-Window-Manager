// data_device.hpp —— 剪贴板 / primary selection（真实现，不是空桩）
//
// 覆盖：
//   wl_data_device_manager (v3)            —— 复制粘贴
//   zwp_primary_selection_device_manager_v1 —— 中键粘贴（X11 传统）
//
// 协议要点（Wayland 里剪贴板是「源端推、目标端拉」）：
//   1. 复制方 create_data_source → 若干个 offer(mime) 声明自己能给什么格式
//      → set_selection(source) 把它设为当前剪贴板；
//   2. 本合成器持有这个 source 资源，并给**其他**客户端的 data_device 发
//      data_offer + offer(mime)… + selection(offer)；
//   3. 粘贴方对 offer 调 receive(mime, fd) —— 这个 fd 是**粘贴方给的管道写端**，
//      我们把它原样转给复制方的 source.send(mime, fd)，
//      两个进程之间就通过这条管道直接搬数据，我们不碰内容。
//      （所以剪贴板内容不经合成器内存，也就能传任意大小/任意格式）
//
// 与拖放的边界：start_drag 目前直接忽略（不做 DnD）。数据源会收到 cancelled
// 以外的沉默 —— 这在只有复制粘贴的场景下不影响任何客户端。
#pragma once

#include <string>
#include <vector>

// ---- 状态 ---------------------------------------------------------------
struct DdSrc {                       // 一个「内容提供者」（wl_data_source 或 primary 版）
    wl_resource *res = nullptr;
    bool primary = false;
    wl_client *client = nullptr;
    std::vector<std::string> mimes;
};

struct DdOffer {                     // 我们发给某个客户端的 wl_data_offer
    wl_resource *res = nullptr;
    DdSrc *src = nullptr;
    bool primary = false;
    std::string accepted;            // accept() 里客户端选中的 mime（仅记录）
};

struct DdDev {                       // 一个客户端的 data_device / primary device
    bool primary = false;
    wl_resource *cur_offer = nullptr;   // 当前已发给它的 offer（换的时候要销毁）
};

static std::vector<DdSrc *> g_dd_srcs;
static std::vector<DdOffer *> g_dd_offers;
static std::vector<std::pair<wl_resource *, DdDev *>> g_dd_devs;
static DdSrc *g_dd_clip = nullptr;    // 当前剪贴板
static DdSrc *g_dd_prim = nullptr;    // 当前 primary selection

static DdSrc *dd_src_of(wl_resource *r) {
    return r ? (DdSrc *)wl_resource_get_user_data(r) : nullptr;
}
static DdOffer *dd_offer_of(wl_resource *r) {
    return r ? (DdOffer *)wl_resource_get_user_data(r) : nullptr;
}

static void dd_broadcast(bool primary);

// ---- wl_data_offer / zwp_primary_selection_offer_v1 ----------------------
static void dd_offer_accept(wl_client *, wl_resource *r, uint32_t, const char *mime) {
    DdOffer *o = dd_offer_of(r);
    if (o) o->accepted = mime ? mime : "";
}
static void dd_offer_receive(wl_client *, wl_resource *r, const char *mime, int32_t fd) {
    DdOffer *o = dd_offer_of(r);
    // 把粘贴方的管道写端直接转给复制方：数据在两端之间流动，我们不留副本
    if (o && o->src && o->src->res && mime) {
        if (o->src->primary)
            zwp_primary_selection_source_v1_send_send(o->src->res, mime, fd);
        else
            wl_data_source_send_send(o->src->res, mime, fd);
    } else {
        printf("[clip] receive 时没有可用的源，丢弃\n");
    }
    close(fd);   // send 时 libwayland 已经 dup 进连接，这里关掉自己那份
}
static void dd_offer_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void dd_offer_set_actions(wl_client *, wl_resource *r, uint32_t, uint32_t) {}
static void dd_offer_res_destroyed(wl_resource *r) {
    DdOffer *o = dd_offer_of(r);
    if (!o) return;
    for (size_t i = 0; i < g_dd_offers.size(); i++)
        if (g_dd_offers[i] == o) { g_dd_offers.erase(g_dd_offers.begin() + i); break; }
    // 设备那边如果还指着它就别留悬空
    for (auto &d : g_dd_devs) {
        DdDev *st = d.second;
        if (st && st->cur_offer == r) st->cur_offer = nullptr;
    }
    delete o;
}

static const struct wl_data_offer_interface dd_doffer_impl = {
    .accept = dd_offer_accept,
    .receive = dd_offer_receive,
    .destroy = dd_offer_destroy,
    .finish = dd_offer_destroy,
    .set_actions = dd_offer_set_actions,
};
static const struct zwp_primary_selection_offer_v1_interface dd_poffer_impl = {
    .receive = dd_offer_receive,
    .destroy = dd_offer_destroy,
};

// ---- wl_data_source / zwp_primary_selection_source_v1 -------------------
static void dd_src_offer(wl_client *, wl_resource *r, const char *mime) {
    DdSrc *s = dd_src_of(r);
    if (s && mime) s->mimes.push_back(mime);
}
static void dd_src_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void dd_src_set_actions(wl_client *, wl_resource *, uint32_t) {}
static void dd_src_res_destroyed(wl_resource *r) {
    DdSrc *s = dd_src_of(r);
    if (!s) return;
    bool was_current = (g_dd_clip == s) || (g_dd_prim == s);
    bool was_primary = s->primary;
    for (size_t i = 0; i < g_dd_srcs.size(); i++)
        if (g_dd_srcs[i] == s) { g_dd_srcs.erase(g_dd_srcs.begin() + i); break; }
    if (g_dd_clip == s) g_dd_clip = nullptr;
    if (g_dd_prim == s) g_dd_prim = nullptr;
    delete s;
    // 源没了 → 所有目标端的剪贴板立刻清空（否则它们会以为还有东西可粘）
    if (was_current) dd_broadcast(was_primary);
}

static const struct wl_data_source_interface dd_dsrc_impl = {
    .offer = dd_src_offer,
    .destroy = dd_src_destroy,
    .set_actions = dd_src_set_actions,
};
static const struct zwp_primary_selection_source_v1_interface dd_psrc_impl = {
    .offer = dd_src_offer,
    .destroy = dd_src_destroy,
};

// ---- 把当前选择推给某个客户端 -------------------------------------------
static void dd_push_to(wl_resource *dev, DdDev *st, DdSrc *src) {
    if (!st) return;
    wl_client *cli = wl_resource_get_client(dev);
    uint32_t ver = wl_resource_get_version(dev);

    if (st->cur_offer) {                       // 先撤掉上一次的 offer
        wl_resource *old = st->cur_offer;
        st->cur_offer = nullptr;
        wl_resource_destroy(old);
    }

    // 复制方自己不该收到自己的 offer；没内容也一样发 selection(NULL) 来清空
    bool give = src && !src->mimes.empty() && src->client != cli;
    if (!give) {
        if (st->primary) zwp_primary_selection_device_v1_send_selection(dev, nullptr);
        else wl_data_device_send_selection(dev, nullptr);
        return;
    }

    DdOffer *o = new DdOffer();
    o->src = src;
    o->primary = st->primary;
    // id 传 0 = 由服务端分配对象号（服务端主动创建的对象都这样）
    o->res = wl_resource_create(cli,
                                st->primary ? &zwp_primary_selection_offer_v1_interface
                                            : &wl_data_offer_interface,
                                ver, 0);
    if (!o->res) { delete o; wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(o->res,
                                   st->primary ? (const void *)&dd_poffer_impl
                                               : (const void *)&dd_doffer_impl,
                                   o, dd_offer_res_destroyed);
    g_dd_offers.push_back(o);

    if (st->primary) {
        for (const std::string &m : src->mimes)
            zwp_primary_selection_offer_v1_send_offer(o->res, m.c_str());
        zwp_primary_selection_device_v1_send_selection(dev, o->res);
    } else {
        for (const std::string &m : src->mimes)
            wl_data_offer_send_offer(o->res, m.c_str());
        wl_data_device_send_selection(dev, o->res);
    }
    st->cur_offer = o->res;
    printf("[clip] 推给客户端 %p：%zu 种 mime（%s）\n", (void *)cli, src->mimes.size(),
           src->mimes[0].c_str());
}

static void dd_broadcast(bool primary) {
    DdSrc *src = primary ? g_dd_prim : g_dd_clip;
    std::vector<std::pair<wl_resource *, DdDev *>> snapshot = g_dd_devs;   // 回调里可能改容器
    for (auto &d : snapshot)
        if (d.second && d.second->primary == primary) dd_push_to(d.first, d.second, src);
}

// ---- wl_data_device / zwp_primary_selection_device_v1 -------------------
static void dd_dev_set_selection(wl_client *, wl_resource *r, wl_resource *source, uint32_t) {
    DdDev *st = (DdDev *)wl_resource_get_user_data(r);
    bool primary = st ? st->primary : false;
    DdSrc *s = dd_src_of(source);
    DdSrc *old = primary ? g_dd_prim : g_dd_clip;
    if (old && old != s) {
        // 告诉被顶掉的源「你不再是所有者了」（它通常随即 destroy）
        if (old->primary) zwp_primary_selection_source_v1_send_cancelled(old->res);
        else wl_data_source_send_cancelled(old->res);
    }
    if (primary) g_dd_prim = s; else g_dd_clip = s;
    printf("[clip] %s selection <- %s（%zu 种 mime）\n",
           primary ? "primary" : "clipboard",
           s ? "有新源" : "(清空)", s ? s->mimes.size() : 0);
    dd_broadcast(primary);
}
static void dd_dev_start_drag(wl_client *, wl_resource *, wl_resource *, wl_resource *,
                              wl_resource *, uint32_t) {
    // 不做 DnD。按协议最稳妥的处理是把源标记取消，客户端就不会一直等。
}
static void dd_dev_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void dd_dev_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wl_data_device_interface dd_ddev_impl = {
    .start_drag = dd_dev_start_drag,
    .set_selection = dd_dev_set_selection,
    .release = dd_dev_release,
};
static const struct zwp_primary_selection_device_v1_interface dd_pdev_impl = {
    .set_selection = dd_dev_set_selection,
    .destroy = dd_dev_destroy,
};

static void dd_dev_res_destroyed(wl_resource *r) {
    for (size_t i = 0; i < g_dd_devs.size(); i++) {
        if (g_dd_devs[i].first == r) {
            delete g_dd_devs[i].second;
            g_dd_devs.erase(g_dd_devs.begin() + i);
            break;
        }
    }
}

// ---- 工厂 ---------------------------------------------------------------
static void dd_create_source(bool primary, wl_client *c, wl_resource *mgr, uint32_t id) {
    uint32_t ver = wl_resource_get_version(mgr);
    wl_resource *r = wl_resource_create(
        c, primary ? &zwp_primary_selection_source_v1_interface : &wl_data_source_interface,
        ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    DdSrc *s = new DdSrc();
    s->res = r;
    s->primary = primary;
    s->client = c;
    g_dd_srcs.push_back(s);
    wl_resource_set_implementation(r, primary ? (const void *)&dd_psrc_impl
                                              : (const void *)&dd_dsrc_impl,
                                   s, dd_src_res_destroyed);
}

static void dd_get_device(bool primary, wl_client *c, wl_resource *mgr, uint32_t id) {
    uint32_t ver = wl_resource_get_version(mgr);
    wl_resource *r = wl_resource_create(
        c, primary ? &zwp_primary_selection_device_v1_interface : &wl_data_device_interface,
        ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    DdDev *st = new DdDev();
    st->primary = primary;
    g_dd_devs.push_back({r, st});
    wl_resource_set_implementation(r, primary ? (const void *)&dd_pdev_impl
                                              : (const void *)&dd_ddev_impl,
                                   st, dd_dev_res_destroyed);
    // 新设备立刻把它该看到的当前选择推过去
    dd_push_to(r, st, primary ? g_dd_prim : g_dd_clip);
}

static void ddm_create_source(wl_client *c, wl_resource *m, uint32_t id) {
    dd_create_source(false, c, m, id);
}
static void ddm_get_device(wl_client *c, wl_resource *m, uint32_t id, wl_resource *) {
    dd_get_device(false, c, m, id);
}
static const struct wl_data_device_manager_interface ddm_impl = {
    .create_data_source = ddm_create_source,
    .get_data_device = ddm_get_device,
};
static void psel_mgr_create_source(wl_client *c, wl_resource *m, uint32_t id) {
    dd_create_source(true, c, m, id);
}
static void psel_mgr_get_device(wl_client *c, wl_resource *m, uint32_t id, wl_resource *) {
    dd_get_device(true, c, m, id);
}
static void psel_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zwp_primary_selection_device_manager_v1_interface psel_mgr_impl = {
    .create_source = psel_mgr_create_source,
    .get_device = psel_mgr_get_device,
    .destroy = psel_mgr_destroy,
};

static void bind_datadev(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 3) version = 3;
    wl_resource *r = wl_resource_create(c, &wl_data_device_manager_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &ddm_impl, nullptr, nullptr);
}
static void bind_primsel(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > zwp_primary_selection_device_manager_v1_interface.version)
        version = zwp_primary_selection_device_manager_v1_interface.version;
    wl_resource *r = wl_resource_create(c, &zwp_primary_selection_device_manager_v1_interface,
                                        version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &psel_mgr_impl, nullptr, nullptr);
}

// ---- 焦点变化：把剪贴板推给新获得键盘焦点的客户端 -----------------------
// 注意：primary selection 不跟焦点走（它是「当前选中的文本」），所以只推普通剪贴板。
static void dd_focus_changed(View *v) {
    wl_client *fc = (v && v->surface) ? wl_resource_get_client(v->surface) : nullptr;
    std::vector<std::pair<wl_resource *, DdDev *>> snapshot = g_dd_devs;
    for (auto &d : snapshot) {
        DdDev *st = d.second;
        if (!st || st->primary) continue;
        bool focused = fc && wl_resource_get_client(d.first) == fc;
        // 失去焦点的客户端收 selection(NULL)，避免它们继续粘到过期内容
        dd_push_to(d.first, st, focused ? g_dd_clip : nullptr);
    }
}
