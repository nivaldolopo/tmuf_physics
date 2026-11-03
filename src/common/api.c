#include "common/sounds.h"
#include "common/api_common.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/compress.h"
#include "common/controls.h"
#include "common/pack_classes.h"

void tmuf_set_error(char *err, size_t err_size, const char *fmt, ...) {
  if (!err || !err_size)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_size, fmt, ap);
  va_end(ap);
}

/* ---- packs ---- */

tmuf_packs *tmuf_packs_open(const char *dir, char *err, size_t err_size) {
  tmuf_packs *p = calloc(1, sizeof *p);
  if (!p) {
    tmuf_set_error(err, err_size, "out of memory");
    return NULL;
  }
  char e[512] = "";
  if (!tmuf_packset_open(&p->set, dir, e, sizeof e)) {
    tmuf_set_error(err, err_size, "%s", e);
    free(p);
    return NULL;
  }
  return p;
}

void tmuf_packs_close(tmuf_packs *p) {
  if (!p)
    return;
  tmuf_packset_close(&p->set);
  free(p);
}

void *tmuf_packs_read(const tmuf_packs *packs, const char *path, size_t *size) {
  if (size)
    *size = 0;
  if (!packs || !path)
    return NULL;
  const tmuf_pack_ref ref = tmuf_packset_find(&packs->set, path);
  if (ref.pack < 0)
    return NULL;
  uint8_t *data = NULL;
  size_t n = 0;
  if (!tmuf_pack_extract(&packs->set.packs[ref.pack], ref.file, &data, &n))
    return NULL;
  if (size)
    *size = n;
  return data;
}

/* A source that keeps a copy of every byte read through it, and passes the
   parser's feedback on. */
typedef struct record_source {
  tmuf_source base;
  tmuf_source *inner;
  uint8_t *data;
  size_t size, cap;
  int failed;
} record_source;

static int record_read(tmuf_source *src, void *out, size_t n) {
  record_source *r = (record_source *)src;
  if (!r->inner->read(r->inner, out, n))
    return 0;
  if (r->size + n > r->cap) {
    size_t cap = r->cap ? r->cap : 4096;
    while (cap < r->size + n)
      cap *= 2;
    uint8_t *data = realloc(r->data, cap);
    if (!data) {
      r->failed = 1;
      return 0;
    }
    r->data = data;
    r->cap = cap;
  }
  memcpy(r->data + r->size, out, n);
  r->size += n;
  return 1;
}

static void record_mix(tmuf_source *src, const uint8_t *bytes, size_t n) {
  record_source *r = (record_source *)src;
  if (r->inner->mix)
    r->inner->mix(r->inner, bytes, n);
}

void *tmuf_packs_read_parsed(const tmuf_packs *packs, const char *path, size_t *size) {
  if (size)
    *size = 0;
  if (!packs || !path)
    return NULL;
  const tmuf_pack_ref ref = tmuf_packset_find(&packs->set, path);
  if (ref.pack < 0)
    return NULL;
  tmuf_pack_stream ps;
  if (!tmuf_pack_stream_open(&ps, &packs->set.packs[ref.pack], ref.file))
    return NULL;
  record_source rec;
  memset(&rec, 0, sizeof rec);
  rec.base.read = record_read;
  rec.base.mix = record_mix;
  rec.inner = &ps.base;
  tmuf_arena arena;
  tmuf_arena_init(&arena);
  tmuf_gbx g;
  tmuf_gbx_init(&g, &rec.base, &arena, tmuf_pack_classes, tmuf_pack_class_count);
  g.feedback = 1;
  int ok = tmuf_gbx_read_header(&g) && tmuf_gbx_read_root(&g) && !g.error && !rec.failed;
  tmuf_arena_free(&arena);
  tmuf_pack_stream_close(&ps);
  if (!ok) {
    free(rec.data);
    return NULL;
  }
  if (size)
    *size = rec.size;
  return rec.data ? rec.data : calloc(1, 1);
}

/* ---- interface frames ---- */

struct tmuf_frame {
  tmuf_arena arena; /* the parsed nodes; the strings point into it */
  tmuf_frame_control *controls;
  size_t count, cap;
};

static int derives_from(uint32_t class_id, uint32_t base) {
  for (int depth = 0; depth < 32 && class_id && class_id != 0xffffffffu; depth++) {
    if (class_id == base)
      return 1;
    class_id = tmuf_class_parent(class_id);
  }
  return 0;
}

static int derives_from_frame(uint32_t class_id) { return derives_from(class_id, 0x07016000u); }

static const char *node_file(const tmuf_gbx_node *n) {
  return n && n->external && n->file ? n->file : "";
}

static void frame_add_child(tmuf_frame *f, const tmuf_control_child *ch, int parent, int depth);

static void frame_add(tmuf_frame *f, const tmuf_gbx_node *n, int parent, const float *iso, const char *instance_name,
                      int depth) {
  if (!n || depth > 32)
    return;
  if (f->count == f->cap) {
    size_t cap = f->cap ? f->cap * 2 : 64;
    tmuf_frame_control *c = realloc(f->controls, cap * sizeof *c);
    if (!c)
      return;
    f->controls = c;
    f->cap = cap;
  }
  int index = (int)f->count++;
  tmuf_frame_control *out = &f->controls[index];
  memset(out, 0, sizeof *out);
  out->parent = parent;
  out->class_id = n->class_id;
  out->class_name = n->cls ? n->cls->name : "";
  out->name = instance_name ? instance_name : "";
  out->file = out->text = out->style = out->font = out->image = out->substyle = "";
  if (iso)
    memcpy(out->iso, iso, sizeof out->iso);
  else
    out->iso[0] = out->iso[4] = out->iso[8] = 1.0f;
  if (n->external) {
    /* an instanced frame file (the parser only reads it as a reference) */
    out->class_id = 0x07016000u;
    out->class_name = "CControlFrame";
    out->file = node_file(n);
    return;
  }
  const tmuf_control *c = derives_from(n->class_id, 0x07001000u) ? n->data : NULL;
  if (!c)
    return;
  if (!out->name[0] && c->name && c->name[0]) /* an instance keeps its own id */
    out->name = c->name;
  memcpy(out->box, c->box, sizeof out->box);
  memcpy(out->align, c->align, sizeof out->align);
  out->flags = c->flags;
  out->text = c->text ? c->text : "";
  out->image = node_file(c->image);
  out->substyle = c->quad_id ? c->quad_id : "";
  const tmuf_control_style *st = NULL;
  if (c->style && c->style->external)
    out->style = node_file(c->style);
  else if (c->style && c->style->data && c->style->class_id == 0x07017000u)
    st = c->style->data;
  if (!out->style[0] && c->style_name && c->style_name[0])
    out->style = c->style_name;
  if (!out->style[0] && c->style_sheet)
    out->style = node_file(c->style_sheet);
  if (c->has_own_style)
    st = &c->own_style;
  if (!out->substyle[0] && st && st->icon)
    out->substyle = st->icon;
  if (st) {
    out->font = node_file(st->font);
    memcpy(out->text_size, st->text_size, sizeof out->text_size);
    out->has_color = st->has_color;
    memcpy(out->color, st->color, sizeof out->color);
    if (!out->style[0] && st->name)
      out->style = st->name;
  }
  for (uint32_t i = 0; i < c->child_count; i++)
    frame_add_child(f, &c->children[i], index, depth + 1);
}

/* A child, then what an instance puts in its model's anchors: each
   anchor's children under the control of that name below the child */
static void frame_add_child(tmuf_frame *f, const tmuf_control_child *ch, int parent, int depth) {
  const size_t first = f->count;
  frame_add(f, ch->control, parent, ch->iso, ch->name, depth);
  for (uint32_t a = 0; a < ch->anchor_count; a++) {
    const tmuf_control *anchor = &ch->anchors[a];
    int at = -1;
    for (size_t i = first; i < f->count && at < 0; i++)
      if (anchor->name && strcmp(f->controls[i].name, anchor->name) == 0)
        at = (int)i;
    if (at < 0)
      continue;
    for (uint32_t i = 0; i < anchor->child_count; i++)
      frame_add_child(f, &anchor->children[i], at, depth + 1);
  }
}

/* Parses a frame (menu 0) or menu (menu 1) file into a new tmuf_frame */
static tmuf_frame *controls_load(const tmuf_packs *packs, const char *path, int menu, char *err, size_t err_size) {
  if (!packs || !path) {
    tmuf_set_error(err, err_size, "no packs or path");
    return NULL;
  }
  const tmuf_pack_ref ref = tmuf_packset_find(&packs->set, path);
  if (ref.pack < 0) {
    tmuf_set_error(err, err_size, "%s: not in the packs", path);
    return NULL;
  }
  const tmuf_pack *pack = &packs->set.packs[ref.pack];
  tmuf_pack_stream ps;
  if (!tmuf_pack_stream_open(&ps, pack, ref.file)) {
    tmuf_set_error(err, err_size, "%s: cannot open", path);
    return NULL;
  }
  tmuf_frame *f = calloc(1, sizeof *f);
  if (!f) {
    tmuf_pack_stream_close(&ps);
    tmuf_set_error(err, err_size, "out of memory");
    return NULL;
  }
  tmuf_arena_init(&f->arena);
  tmuf_gbx g;
  tmuf_gbx_init(&g, &ps.base, &f->arena, tmuf_pack_classes, tmuf_pack_class_count);
  g.feedback = 1;
  const void *root = tmuf_gbx_read_header(&g) ? tmuf_gbx_read_root(&g) : NULL;
  const uint64_t size = pack->files[ref.file].uncompressed_size;
  tmuf_pack_stream_close(&ps);
  if (!root || g.error || g.pos != size) {
    tmuf_set_error(err, err_size, "%s: %s after %llu of %llu bytes", path, g.error ? g.message : "parse ended",
                   (unsigned long long)g.pos, (unsigned long long)size);
    tmuf_frame_free(f);
    return NULL;
  }
  if (menu ? g.class_id != 0x03009000u : !derives_from_frame(g.class_id)) {
    tmuf_set_error(err, err_size, "%s: not a %s (%08x)", path, menu ? "menu" : "frame", g.class_id);
    tmuf_frame_free(f);
    return NULL;
  }
  if (!menu) {
    frame_add(f, &g.nodes[0], -1, NULL, NULL, 0);
    return f;
  }
  const tmuf_game_menu *m = root;
  for (uint32_t i = 0; i < m->frame_count; i++)
    frame_add_child(f, &m->frames[i], -1, 0);
  return f;
}

tmuf_frame *tmuf_frame_load(const tmuf_packs *packs, const char *path, char *err, size_t err_size) {
  return controls_load(packs, path, 0, err, err_size);
}

tmuf_frame *tmuf_menu_load(const tmuf_packs *packs, const char *path, char *err, size_t err_size) {
  return controls_load(packs, path, 1, err, err_size);
}

size_t tmuf_frame_control_count(const tmuf_frame *f) { return f ? f->count : 0; }

const tmuf_frame_control *tmuf_frame_controls(const tmuf_frame *f) { return f ? f->controls : NULL; }

void tmuf_frame_free(tmuf_frame *f) {
  if (!f)
    return;
  tmuf_arena_free(&f->arena);
  free(f->controls);
  free(f);
}

/* The style's root read from src (size bytes) into out. */
static int read_control_style(tmuf_source *src, int feedback, uint64_t size, const char *path,
                              tmuf_control_style_info *out, char *err, size_t err_size) {
  tmuf_arena arena;
  tmuf_arena_init(&arena);
  tmuf_gbx g;
  tmuf_gbx_init(&g, src, &arena, tmuf_pack_classes, tmuf_pack_class_count);
  g.feedback = feedback;
  const tmuf_control_style *st = tmuf_gbx_read_header(&g) ? tmuf_gbx_read_root(&g) : NULL;
  int ok = 0;
  if (!st || g.error || g.pos != size)
    tmuf_set_error(err, err_size, "%s: %s after %llu of %llu bytes", path, g.error ? g.message : "parse ended",
                   (unsigned long long)g.pos, (unsigned long long)size);
  else if (g.class_id != 0x07017000u)
    tmuf_set_error(err, err_size, "%s: not a control style (%08x)", path, g.class_id);
  else {
    snprintf(out->font, sizeof out->font, "%s", node_file(st->font));
    memcpy(out->text_size, st->text_size, sizeof out->text_size);
    out->has_color = st->has_color;
    memcpy(out->color, st->color, sizeof out->color);
    snprintf(out->format, sizeof out->format, "%s", st->format ? st->format : "");
    ok = 1;
  }
  tmuf_arena_free(&arena);
  return ok;
}

int tmuf_control_style_load(const tmuf_packs *packs, const char *path, tmuf_control_style_info *out, char *err,
                            size_t err_size) {
  if (out)
    memset(out, 0, sizeof *out);
  if (!packs || !path || !out) {
    tmuf_set_error(err, err_size, "no packs, path or output");
    return 0;
  }
  const tmuf_pack_ref ref = tmuf_packset_find(&packs->set, path);
  if (ref.pack < 0) {
    /* the game ships some styles loose under GameData (e.g.
       MenuForever\Media\ControlStyle\TextTitle3.ControlStyle.Gbx), plain
       and unencrypted */
    char disk[1200];
    if (!tmuf_packset_find_file(&packs->set, path, disk, sizeof disk)) {
      tmuf_set_error(err, err_size, "%s: not in the packs or GameData", path);
      return 0;
    }
    FILE *f = fopen(disk, "rb");
    uint8_t *data = NULL;
    long n = -1;
    if (f && fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (data = malloc((size_t)n)) != NULL && fread(data, 1, (size_t)n, f) != (size_t)n)
      n = -1;
    if (f)
      fclose(f);
    int ok = 0;
    if (!data || n <= 0)
      tmuf_set_error(err, err_size, "%s: cannot read %s", path, disk);
    else {
      tmuf_mem_source ms;
      tmuf_mem_source_init(&ms, data, (size_t)n);
      ok = read_control_style(&ms.base, 0, (uint64_t)n, path, out, err, err_size);
    }
    free(data);
    return ok;
  }
  const tmuf_pack *pack = &packs->set.packs[ref.pack];
  tmuf_pack_stream ps;
  if (!tmuf_pack_stream_open(&ps, pack, ref.file)) {
    tmuf_set_error(err, err_size, "%s: cannot open", path);
    return 0;
  }
  const int ok = read_control_style(&ps.base, 1, pack->files[ref.file].uncompressed_size, path, out, err, err_size);
  tmuf_pack_stream_close(&ps);
  return ok;
}

/* A file parsed whole from the packs or, when they don't have it, from
   GameData (plain), its nodes kept in arena until parsed_free */
typedef struct parsed_file {
  tmuf_arena arena;
  tmuf_gbx g;
  uint8_t *data; /* a GameData file's bytes */
  char dir[260]; /* its folder, ending in '\\' (for its references) */
  const void *root;
} parsed_file;

static void parsed_free(parsed_file *p) {
  tmuf_arena_free(&p->arena);
  free(p->data);
}

static int parse_path(const tmuf_packs *packs, const char *path, parsed_file *p, char *err, size_t err_size) {
  memset(p, 0, sizeof *p);
  tmuf_arena_init(&p->arena);
  snprintf(p->dir, sizeof p->dir, "%s", path);
  char *slash = strrchr(p->dir, '\\');
  if (slash)
    slash[1] = 0;
  else
    p->dir[0] = 0;
  uint64_t size = 0;
  const tmuf_pack_ref ref = tmuf_packset_find(&packs->set, path);
  tmuf_pack_stream ps;
  tmuf_mem_source ms;
  int stream = 0;
  if (ref.pack >= 0) {
    if (!tmuf_pack_stream_open(&ps, &packs->set.packs[ref.pack], ref.file)) {
      tmuf_set_error(err, err_size, "%s: cannot open", path);
      return 0;
    }
    stream = 1;
    size = packs->set.packs[ref.pack].files[ref.file].uncompressed_size;
    tmuf_gbx_init(&p->g, &ps.base, &p->arena, tmuf_pack_classes, tmuf_pack_class_count);
    p->g.feedback = 1;
  } else {
    char disk[1200];
    FILE *f = tmuf_packset_find_file(&packs->set, path, disk, sizeof disk) ? fopen(disk, "rb") : NULL;
    long n = -1;
    if (f && fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (p->data = malloc((size_t)n)) != NULL && fread(p->data, 1, (size_t)n, f) != (size_t)n)
      n = -1;
    if (f)
      fclose(f);
    if (!p->data || n <= 0) {
      tmuf_set_error(err, err_size, "%s: not in the packs or GameData", path);
      return 0;
    }
    size = (uint64_t)n;
    tmuf_mem_source_init(&ms, p->data, (size_t)n);
    tmuf_gbx_init(&p->g, &ms.base, &p->arena, tmuf_pack_classes, tmuf_pack_class_count);
  }
  p->root = tmuf_gbx_read_header(&p->g) ? tmuf_gbx_read_root(&p->g) : NULL;
  if (stream)
    tmuf_pack_stream_close(&ps);
  if (!p->root || p->g.error || p->g.pos != size) {
    tmuf_set_error(err, err_size, "%s: %s after %llu of %llu bytes", path, p->g.error ? p->g.message : "parse ended",
                   (unsigned long long)p->g.pos, (unsigned long long)size);
    return 0;
  }
  return 1;
}

static int ends_with(const char *s, const char *tail) {
  const size_t n = strlen(s), t = strlen(tail);
  if (n < t)
    return 0;
  for (size_t i = 0; i < t; i++)
    if (tolower((unsigned char)s[n - t + i]) != tolower((unsigned char)tail[i]))
      return 0;
  return 1;
}

/* The plain path of p's first external reference whose name ends in tail */
static int external_ending(const parsed_file *p, const char *tail, char *out, size_t out_size) {
  for (uint32_t i = 1; i <= p->g.node_count; i++) {
    const tmuf_gbx_node *n = &p->g.nodes[i];
    if (n->external && n->file && ends_with(n->file, tail))
      return tmuf_gbx_external_path(&p->g, n, p->dir, out, out_size);
  }
  return 0;
}

/* The first picture a material's files name, a few files deep */
static int find_picture(const tmuf_packs *packs, const char *path, int depth, char *out, size_t out_size) {
  if (depth > 4)
    return 0;
  parsed_file p;
  char err[8];
  int found = 0;
  if (parse_path(packs, path, &p, err, sizeof err) || p.g.node_count) {
    for (uint32_t i = 1; i <= p.g.node_count && !found; i++) {
      const tmuf_gbx_node *n = &p.g.nodes[i];
      char next[260];
      if (!n->external || !n->file || !tmuf_gbx_external_path(&p.g, n, p.dir, next, sizeof next))
        continue;
      if (ends_with(n->file, ".dds") || ends_with(n->file, ".tga")) {
        snprintf(out, out_size, "%s", next);
        found = 1;
      } else if (ends_with(n->file, ".Gbx")) {
        found = find_picture(packs, next, depth + 1, out, out_size);
      }
    }
  }
  parsed_free(&p);
  return found;
}

int tmuf_control_style_art_load(const tmuf_packs *packs, const char *path, tmuf_control_style_art *out, char *err,
                                size_t err_size) {
  if (out)
    memset(out, 0, sizeof *out);
  if (!packs || !path || !out) {
    tmuf_set_error(err, err_size, "no packs, path or output");
    return 0;
  }
  parsed_file p;
  if (!parse_path(packs, path, &p, err, err_size)) {
    parsed_free(&p);
    return 0;
  }
  if (p.g.class_id != 0x07017000u) {
    tmuf_set_error(err, err_size, "%s: not a control style", path);
    parsed_free(&p);
    return 0;
  }
  const tmuf_control_style *st = p.root;
  snprintf(out->icon, sizeof out->icon, "%s", st->icon ? st->icon : "");
  /* the enum is its own file (Bgs1) or inside the style (BgRaceScore2) */
  const int own = !external_ending(&p, ".FuncEnum.Gbx", out->func_enum, sizeof out->func_enum);
  if (own)
    snprintf(out->func_enum, sizeof out->func_enum, "%s", path);
  parsed_free(&p);
  if (!parse_path(packs, out->func_enum, &p, err, err_size)) {
    parsed_free(&p);
    return 0;
  }
  const tmuf_func_enum *e = p.g.class_id == 0x0500e000u ? p.root : NULL;
  for (uint32_t i = 1; i <= p.g.node_count && !e; i++)
    if (!p.g.nodes[i].external && p.g.nodes[i].data && p.g.nodes[i].class_id == 0x0500e000u)
      e = p.g.nodes[i].data;
  char icons[260] = "";
  if (!e || !(external_ending(&p, ".Material.Gbx", out->material, sizeof out->material) ||
              external_ending(&p, ".Texture.Gbx", out->material, sizeof out->material))) {
    tmuf_set_error(err, err_size, "%s: no CFuncEnum with a material", out->func_enum);
    parsed_free(&p);
    return 0;
  }
  out->columns = e->columns;
  out->rows = e->rows;
  memcpy(out->uv_min, e->uv_min, sizeof out->uv_min);
  memcpy(out->uv_max, e->uv_max, sizeof out->uv_max);
  memcpy(out->size, e->size, sizeof out->size);
  external_ending(&p, ".RefBuffer.Gbx", icons, sizeof icons);
  /* the icons: inline in the enum's file (BgsPlayerCard), or in a
     RefBuffer file of their own (Bgs1IconIndexs) */
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 1) {
      parsed_free(&p);
      if (!icons[0])
        break;
      if (!parse_path(packs, icons, &p, err, err_size)) {
        parsed_free(&p);
        return 0;
      }
    }
    for (uint32_t i = 1; i <= p.g.node_count && out->icon_count < 256; i++) {
      const tmuf_gbx_node *n = &p.g.nodes[i];
      if (n->external || !n->data || n->class_id != 0x0702b000u)
        continue;
      const tmuf_icon_index *ix = n->data;
      tmuf_style_icon *o = &out->icons[out->icon_count++];
      snprintf(o->name, sizeof o->name, "%s", ix->name ? ix->name : "");
      memcpy(o->numbers, ix->numbers, sizeof o->numbers);
      o->index = ix->index;
      o->extra = ix->extra;
      memcpy(o->reals, ix->reals, sizeof o->reals);
    }
    if (pass == 1)
      parsed_free(&p);
  }
  if (!find_picture(packs, out->material, 0, out->picture, sizeof out->picture)) {
    tmuf_set_error(err, err_size, "%s: no picture", out->material);
    return 0;
  }
  return 1;
}

/* ---- replays ---- */

tmuf_replay *tmuf_replay_load(const void *data, size_t size, char *err, size_t err_size) {
  return tmuf_replay_load_ghost(data, size, 0, err, err_size);
}

tmuf_replay *tmuf_replay_load_ghost(const void *data, size_t size, uint32_t ghost, char *err, size_t err_size) {
  tmuf_replay *r = calloc(1, sizeof *r);
  if (!r || !(r->data = malloc(size ? size : 1))) {
    free(r);
    tmuf_set_error(err, err_size, "out of memory");
    return NULL;
  }
  memcpy(r->data, data, size);
  tmuf_arena_init(&r->arena);
  char e[512] = "";
  if (!tmuf_replay_parse(r->data, size, &r->arena, &r->file, e, sizeof e) || !r->file.challenge ||
      r->file.ghost_count == 0) {
    tmuf_set_error(err, err_size, "replay: %s", e[0] ? e : "no map or ghost");
    tmuf_replay_free(r);
    return NULL;
  }
  if (ghost >= r->file.ghost_count) {
    tmuf_set_error(err, err_size, "replay: no ghost %u (it holds %u)", ghost, r->file.ghost_count);
    tmuf_replay_free(r);
    return NULL;
  }
  r->ghost = r->file.ghosts[ghost];
  if (r->ghost->has_inputs) {
    tmuf_control_tick *ticks = NULL;
    uint32_t n = tmuf_control_ticks(r->ghost, &ticks);
    r->inputs = calloc(n ? n : 1, sizeof *r->inputs);
    if (!r->inputs) {
      free(ticks);
      tmuf_set_error(err, err_size, "out of memory");
      tmuf_replay_free(r);
      return NULL;
    }
    for (uint32_t i = 0; i < n; i++) {
      r->inputs[i].accelerate = ticks[i].accelerate;
      r->inputs[i].brake = ticks[i].brake;
      r->inputs[i].steer = ticks[i].steer;
      r->inputs[i].respawn = ticks[i].respawns != 0;
      r->inputs[i].input_event = ticks[i].input_event;
    }
    r->input_count = n;
    free(ticks);
  }
  return r;
}

void tmuf_replay_free(tmuf_replay *r) {
  if (!r)
    return;
  free(r->inputs);
  tmuf_arena_free(&r->arena);
  free(r->data);
  free(r);
}

uint32_t tmuf_replay_ghost_count(const tmuf_replay *r) { return r->file.ghost_count; }

const void *tmuf_replay_map(const tmuf_replay *r, size_t *size) {
  if (size)
    *size = r->file.challenge_size;
  return r->file.challenge;
}

const char *tmuf_replay_vehicle(const tmuf_replay *r) { return r->ghost->vehicle[0] ? r->ghost->vehicle[0] : ""; }
const char *tmuf_replay_skin(const tmuf_replay *r) { return r->ghost->skin ? r->ghost->skin : ""; }

uint32_t tmuf_replay_laps(const tmuf_replay *r) { return r->ghost->settings_laps; }

uint32_t tmuf_replay_seed(const tmuf_replay *r) {
  return r->ghost->has_validation_seed ? r->ghost->validation_seed : 0u;
}

uint32_t tmuf_replay_race_time(const tmuf_replay *r) {
  return r->ghost->has_race_time ? r->ghost->race_time : UINT32_MAX;
}

uint32_t tmuf_replay_respawns(const tmuf_replay *r) {
  return r->ghost->has_respawns ? r->ghost->respawns : UINT32_MAX;
}

uint32_t tmuf_replay_stunt_score(const tmuf_replay *r) {
  return r->ghost->has_stunt_score ? r->ghost->stunt_score : UINT32_MAX;
}

uint32_t tmuf_replay_checkpoints(const tmuf_replay *r, const uint32_t **times, const uint32_t **scores) {
  if (times)
    *times = r->ghost->checkpoint_times;
  if (scores)
    *scores = r->ghost->checkpoint_scores;
  return r->ghost->checkpoint_count;
}

uint32_t tmuf_replay_inputs(const tmuf_replay *r, const tmuf_input **inputs) {
  if (inputs)
    *inputs = r->inputs;
  return r->input_count;
}

uint32_t tmuf_replay_horns(const tmuf_replay *r, uint32_t *ticks, uint32_t max) {
  return r && r->ghost ? tmuf_control_horns(r->ghost, ticks, max) : 0;
}

/* ---- tracks (backend independent part) ---- */

/* The scene's CSceneMobilLeaves: its first leaf emitter's manager model
   (CMotionManagerLeaves, Rally\MotionManagerLeaves\RallyLeafManager.Gbx)
   and the leaf mobil it holds; NULL without emitters */
static const tmuf_scene_mobil_leaves *leaf_mobil(tmuf_scene *s) {
  if (!s->leaf_emitter_count || !s->leaf_manager)
    return NULL;
  tmuf_asset *ma;
  tmuf_gbx_node *mn = tmuf_assets_follow(&s->assets, s->leaf_manager_owner, s->leaf_manager, &ma);
  if (!mn || !mn->data || mn->class_id != 0x0804d000u)
    return NULL;
  const tmuf_motion_manager_leaves *m = mn->data;
  tmuf_asset *la;
  tmuf_gbx_node *ln = m->mobil ? tmuf_assets_follow(&s->assets, ma, m->mobil, &la) : NULL;
  if (!ln || !ln->data || ln->class_id != 0x0a05e000u)
    return NULL;
  s->leaf_manager_owner = la; /* the shader's references are the leaf mobil's file's */
  return ln->data;
}

static int add_material_entry(tmuf_scene *s, const tmuf_scene_mobil_leaves *l) {
  if (s->visual_count == s->visual_cap) {
    const uint32_t cap = s->visual_cap ? s->visual_cap + 1 : 1;
    tmuf_scene_visual *v = realloc(s->visuals, sizeof *v * cap);
    if (!v)
      return 0;
    s->visuals = v;
    s->visual_cap = cap;
  }
  tmuf_scene_visual *v = &s->visuals[s->visual_count++];
  memset(v, 0, sizeof *v);
  v->owner = s->leaf_manager_owner;
  v->shader = l->shader;
  tmuf_iso_identity(&v->iso);
  v->corpus = UINT32_MAX;
  v->mip = UINT32_MAX;
  return 1;
}

/* the scene's sound sources with their sounds (each built once) */
static void scene_sounds_build(tmuf_track_base *b) {
  const tmuf_scene *s = &b->scene;
  b->scene_sound_count = 0;
  b->scene_sounds = TMUF_ARENA_ARRAY(&b->arena, tmuf_scene_sound, s->sound_source_count ? s->sound_source_count : 1);
  if (!b->scene_sounds)
    return;
  for (uint32_t i = 0; i < s->sound_source_count; i++) {
    const tmuf_scene_sound_source *src = &s->sound_sources[i];
    const tmuf_sound *sound = NULL;
    for (uint32_t j = 0; j < i && !sound; j++)
      if (s->sound_sources[j].hms->sound == src->hms->sound && s->sound_sources[j].owner == src->owner)
        sound = b->scene_sounds[j].sound;
    if (!sound)
      sound = tmuf_sound_build(&b->scene.assets, src->owner, src->hms->sound, &b->arena);
    tmuf_scene_sound *out = &b->scene_sounds[b->scene_sound_count++];
    memset(out, 0, sizeof *out);
    out->sound = sound;
    for (int r = 0; r < 3; r++)
      for (int c = 0; c < 3; c++)
        out->location.r.m[r][c] = src->iso.m[r][c];
    out->location.t = (tmuf_vec3){src->iso.t[0], src->iso.t[1], src->iso.t[2]};
    memcpy(out->volumic_size, src->hms->volumic_size, sizeof out->volumic_size);
    out->volume = src->hms->has_volume ? src->hms->volume : 1.0f;
    out->pitch = src->hms->has_volume ? src->hms->pitch : 1.0f;
    out->on = src->on;
    out->block_first = 1;
    for (uint32_t j = 0; j < i && out->block_first; j++)
      out->block_first = s->sound_sources[j].tag != src->tag;
    out->block = src->tag;
  }
}

uint32_t tmuf_track_scene_sounds(const tmuf_track *track, const tmuf_scene_sound **sounds) {
  const tmuf_track_base *b = track ? tmuf_track_base_of(track) : NULL;
  if (sounds)
    *sounds = b && b->has_visuals ? b->scene_sounds : NULL;
  return b && b->has_visuals ? b->scene_sound_count : 0;
}

const tmuf_scene_sound *tmuf_track_trigger_sound(const tmuf_track *track, int32_t corpus) {
  const tmuf_track_base *b = track ? tmuf_track_base_of(track) : NULL;
  if (!b || !b->has_visuals || corpus < 0 || (uint32_t)corpus >= b->scene.corpus_count)
    return NULL;
  const uint32_t tag = b->scene.corpora[corpus].tag;
  for (uint32_t i = 0; i < b->scene_sound_count; i++)
    if (b->scene_sounds[i].block == tag && b->scene_sounds[i].block_first && b->scene_sounds[i].sound)
      return &b->scene_sounds[i];
  return NULL;
}

static int leaves_fill(tmuf_leaves *out, const tmuf_scene *s, const tmuf_scene_mobil_leaves *l, uint32_t material,
                       tmuf_arena *arena) {
  tmuf_leaf_emitter *e = TMUF_ARENA_ARRAY(arena, tmuf_leaf_emitter, s->leaf_emitter_count);
  if (!e)
    return 0;
  for (uint32_t i = 0; i < s->leaf_emitter_count; i++) {
    memcpy(e[i].center, s->leaf_emitters[i].center, sizeof e[i].center);
    memcpy(e[i].half, s->leaf_emitters[i].half, sizeof e[i].half);
    e[i].block = s->leaf_emitters[i].tag;
  }
  *out = (tmuf_leaves){material,
                       l->radius,
                       l->radius_random,
                       l->max_count,
                       l->emitter_max_count,
                       l->fall,
                       l->fall_random,
                       l->alpha_speed_max,
                       l->beta_speed_max,
                       l->swing_rate,
                       l->swing_rate_random,
                       l->swing_radius,
                       l->swing_radius_random,
                       {l->wind[0], l->wind[1], l->wind[2]},
                       l->respawn_period,
                       l->far_z,
                       l->curvature,
                       s->leaf_emitter_count,
                       e};
  return 1;
}

int tmuf_track_base_load(tmuf_track_base *b, const tmuf_packs *packs, const void *map, size_t size,
                         const tmuf_track_options *options, char *err, size_t err_size) {
  memset(b, 0, sizeof *b);
  tmuf_arena_init(&b->arena);
  if (!(b->map_data = malloc(size ? size : 1))) {
    tmuf_set_error(err, err_size, "out of memory");
    return 0;
  }
  memcpy(b->map_data, map, size);
  b->map_size = size;
  b->seed = options ? options->seed : 0;
  char e[600] = "";
  if (!tmuf_challenge_parse(b->map_data, size, &b->arena, &b->map, e, sizeof e)) {
    tmuf_set_error(err, err_size, "map: %s", e);
    return 0;
  }
  const unsigned scene_flags = (options && (options->flags & TMUF_TRACK_TRIANGLES) ? TMUF_SCENE_TRIANGLES : 0u) |
                               (options && (options->flags & TMUF_TRACK_VISUALS) ? TMUF_SCENE_VISUALS : 0u);
  if (!tmuf_scene_build(&b->scene, &packs->set, &b->map, scene_flags)) {
    tmuf_set_error(err, err_size, "scene: %s", b->scene.error);
    return 0;
  }
  /* the car: asked for, else the map's, else the environment's; a vehicle
     the packs lack (custom ids) runs as the environment's car; the asked
     name copied (the caller's, a replay's, may go once the track is made) */
  const char *name = options && options->vehicle && options->vehicle[0]
                         ? tmuf_arena_strndup(&b->arena, options->vehicle, strlen(options->vehicle))
                     : b->map.vehicle[0] && b->map.vehicle[0][0]      ? b->map.vehicle[0]
                                                                      : b->scene.default_vehicle;
  if (!name || !tmuf_vehicle_load(&b->vehicle, &b->scene.assets, name, e, sizeof e)) {
    name = b->scene.default_vehicle;
    if (!name || !tmuf_vehicle_load(&b->vehicle, &b->scene.assets, name, e, sizeof e)) {
      tmuf_set_error(err, err_size, "vehicle: %s", e);
      return 0;
    }
  }
  b->vehicle_name = name;
  /* CTrackManiaRace::InitNbLapsAndCheckpoints: the race settings' laps (a
     replay's, see tmuf_replay_laps), else the map's for a lap race, else one */
  b->laps = options && options->laps ? options->laps : b->map.has_laps && b->map.lap_race ? b->map.laps : 1;
  if (scene_flags & TMUF_SCENE_VISUALS) {
    tmuf_weather_build(&b->weather, &b->scene, &b->arena);
    const int is_night = b->weather.found && b->weather.view.is_night;
    const char *mood_folder = b->weather.found ? b->weather.view.mood.folder : NULL;
    /* the leaf mobil's shader as a material of the map's visuals: an entry
       without a visual at the end of the list */
    const tmuf_scene_mobil_leaves *leaves = leaf_mobil(&b->scene);
    uint32_t leaf_entry = UINT32_MAX, *entry_material = NULL;
    if (leaves && leaves->shader && add_material_entry(&b->scene, leaves)) {
      leaf_entry = b->scene.visual_count - 1;
      entry_material = malloc(sizeof *entry_material * b->scene.visual_count);
    }
    const int built = tmuf_lightmap_build(&b->lightmap, &b->scene, 2048) &&
                      tmuf_visuals_build(&b->visuals, &b->scene, b->lightmap.of_scene_corpus, is_night, &b->arena,
                                         entry_material);
    if (built && leaves)
      b->has_leaves = leaves_fill(&b->leaves, &b->scene, leaves,
                                  entry_material && leaf_entry != UINT32_MAX ? entry_material[leaf_entry] : UINT32_MAX,
                                  &b->arena);
    free(entry_material);
    if (!built ||
        !tmuf_vehicle_visuals_build(&b->vehicle_visuals, &b->scene, &b->vehicle, is_night, mood_folder, &b->arena) ||
        !tmuf_track_lights_build(&b->lights, &b->scene, is_night, mood_folder, &b->arena) ||
        !tmuf_scenery_build(&b->scenery, &b->scene, &b->visuals.view, b->weather.found ? &b->weather.view : NULL,
                            b->lights.lights, b->lights.count, &b->arena)) {
      tmuf_set_error(err, err_size, "out of memory");
      return 0;
    }
    scene_sounds_build(b);
    b->has_visuals = 1;
    free(b->scene.visuals); /* the list is not needed any more */
    b->scene.visuals = NULL;
    b->scene.visual_count = b->scene.visual_cap = 0;
  }
  b->triangle_count = b->scene.triangle_count;
  b->triangles = malloc(sizeof *b->triangles * (b->triangle_count ? b->triangle_count : 1));
  if (!b->triangles) {
    tmuf_set_error(err, err_size, "out of memory");
    return 0;
  }
  for (uint32_t i = 0; i < b->triangle_count; i++) {
    memcpy(b->triangles[i].v, b->scene.triangles[i].v, sizeof b->triangles[i].v);
    b->triangles[i].block = b->scene.triangles[i].block;
  }
  free(b->scene.triangles);
  b->scene.triangles = NULL;
  b->scene.triangle_count = b->scene.triangle_cap = 0;
  return 1;
}

const char *tmuf_track_decoration(const tmuf_track *track) {
  const tmuf_track_base *b = track ? tmuf_track_base_of(track) : NULL;
  return b && b->map.decoration[0] ? b->map.decoration[0] : "";
}

const tmuf_visuals *tmuf_track_visuals(const tmuf_track *track) {
  const tmuf_track_base *b = track ? tmuf_track_base_of(track) : NULL;
  return b && b->has_visuals ? &b->visuals.view : NULL;
}

const tmuf_leaves *tmuf_track_leaves(const tmuf_track *track) {
  const tmuf_track_base *b = track ? tmuf_track_base_of(track) : NULL;
  return b && b->has_visuals && b->has_leaves ? &b->leaves : NULL;
}

const tmuf_weather *tmuf_track_weather(const tmuf_track *track) {
  const tmuf_track_base *b = track ? tmuf_track_base_of(track) : NULL;
  return b && b->has_visuals && b->weather.found ? &b->weather.view : NULL;
}

int tmuf_track_camera_water(const tmuf_track *track, tmuf_camera_water *out) {
  const tmuf_track_base *b = track ? tmuf_track_base_of(track) : NULL;
  if (!out)
    return 0;
  memset(out, 0, sizeof *out);
  const tmuf_scene *s = b ? &b->scene : NULL;
  const tmuf_collection *coll = s ? s->collection_info : NULL;
  if (!coll)
    return 0;
  const tmuf_scene_water *w = &s->water;
  /* UpdateWaterMap: the collection's square, the map's size, the heights
     over (base + 1) squares (the cull height as it is); outside wet for a
     default-water collection without plane water */
  const float base = (float)(s->base_height + 1u) * s->square_height;
  out->cell_size[0] = out->cell_size[1] = s->square_size;
  out->dims[0] = w->enabled ? w->dims[0] : s->size[0];
  out->dims[1] = w->enabled ? w->dims[1] : s->size[2];
  out->outside = coll->default_water && !coll->geometry_water_planes ? 1 : 0;
  out->cells = w->enabled ? w->cells : NULL;
  out->top = base + coll->water_surface;
  out->bottom = base + coll->water_secondary;
  out->cull = coll->water_render_cull;
  /* plane mode: the zone's water from its blocks' planes (zone+0x108 =
     SquareHeight) */
  out->plane_mode = coll->geometry_water_planes && s->water_plane_count > 0;
  out->plane_count = s->water_plane_count;
  out->plane_levels = s->water_plane_level;
  return 1;
}

uint32_t tmuf_track_ingame_clips(const tmuf_track *track, const tmuf_ingame_clip **clips) {
  const tmuf_track_base *b = track ? tmuf_track_base_of(track) : NULL;
  if (clips)
    *clips = b && b->map.ingame_clip_count ? b->map.ingame_clips : NULL;
  return b ? b->map.ingame_clip_count : 0;
}

/* CGameCtnChallenge::GetCoordFromPos: the collection's SquareSize (x, z) and
   SquareHeight (y) */
void tmuf_track_trigger_cell_size(const tmuf_track *track, float *xz, float *y) {
  const tmuf_track_base *b = track ? tmuf_track_base_of(track) : NULL;
  if (xz)
    *xz = b ? b->scene.square_size : 0.0f;
  if (y)
    *y = b ? b->scene.square_height : 0.0f;
}

const tmuf_lightmap *tmuf_track_lightmap(const tmuf_track *track) {
  const tmuf_track_base *b = track ? tmuf_track_base_of(track) : NULL;
  return b && b->has_visuals ? &b->lightmap.view : NULL;
}

/* Std.PointsInSphere.Gbx (CPlugPointsInSphereOpt) as the pack */
#define POINTS_IN_SPHERE_FILE "Techno\\Media\\529F0C306598AC140409474EA21F378A3F"

uint32_t tmuf_track_sphere_points(const tmuf_track *track, uint32_t count, const float **points) {
  tmuf_track_base *b = track ? (tmuf_track_base *)tmuf_track_base_of(track) : NULL;
  if (points)
    *points = NULL;
  if (!b)
    return 0;
  tmuf_asset *a = tmuf_assets_load_path(&b->scene.assets, POINTS_IN_SPHERE_FILE);
  const tmuf_points_in_sphere *p = a && a->root && a->class_id == 0x09066000u ? a->root : NULL;
  if (!p || !p->pack_count)
    return 0;
  /* GetPointsInSphereCloseCount: lo the last pack below count, hi the first
     at or above it; the closer one, lo on a tie */
  uint32_t hi = 0;
  while (hi < p->pack_count && p->packs[2 * hi] < count)
    hi++;
  uint32_t k = hi;
  if (hi == p->pack_count)
    k = p->pack_count - 1;
  else if (hi > 0 && count - p->packs[2 * (hi - 1)] <= p->packs[2 * hi] - count)
    k = hi - 1;
  const uint32_t n = p->packs[2 * k], first = p->packs[2 * k + 1];
  if ((uint64_t)first + n > p->point_count)
    return 0;
  if (points)
    *points = p->points + 3u * first;
  return n;
}

uint32_t tmuf_track_lights(const tmuf_track *track, const tmuf_light **lights) {
  const tmuf_track_base *b = track ? tmuf_track_base_of(track) : NULL;
  if (!b || !b->has_visuals) {
    if (lights)
      *lights = NULL;
    return 0;
  }
  if (lights)
    *lights = b->lights.lights;
  return b->lights.count;
}

const tmuf_block_skin *tmuf_track_block_skin(const tmuf_track *track, uint32_t block) {
  const tmuf_track_base *b = track ? tmuf_track_base_of(track) : NULL;
  if (!b || block >= b->scene.block_skin_count || !b->scene.block_skins[block].file)
    return NULL;
  return &b->scene.block_skins[block];
}

const tmuf_scenery_light *tmuf_track_scenery_light(const tmuf_track *track) {
  const tmuf_track_base *b = track ? tmuf_track_base_of(track) : NULL;
  return b && b->has_visuals && b->scenery.found ? &b->scenery.view : NULL;
}

uint32_t tmuf_track_prelight_instance(const tmuf_track *track, uint32_t instance, uint8_t *bgra) {
  const tmuf_track_base *b = track ? tmuf_track_base_of(track) : NULL;
  if (!b || !b->has_visuals || !b->scenery.found || instance >= b->visuals.view.instance_count)
    return 0;
  const tmuf_visuals *v = &b->visuals.view;
  const tmuf_visual_instance *in = &v->instances[instance];
  if (in->mesh >= v->mesh_count)
    return 0;
  const tmuf_visual_mesh *m = &v->meshes[in->mesh];
  if (!(m->flags & TMUF_VISUAL_NORMAL) || (m->flags & TMUF_VISUAL_SPRITES))
    return 0;
  const uint32_t flags = in->material < v->material_count ? v->materials[in->material].generic_flags : 0u;
  const float (*r)[3] = in->location.r.m;
  const float t[3] = {in->location.t.x, in->location.t.y, in->location.t.z};
  for (uint32_t i = 0; i < m->vertex_count; i++) {
    const uint8_t *vx = m->vertices + (size_t)i * m->vertex_stride;
    float p[3], packed_n[3], wp[3], wn[3];
    uint32_t pn;
    memcpy(p, vx, sizeof p);
    memcpy(&pn, vx + 12, sizeof pn);
    for (int k = 0; k < 3; k++) {
      const int32_t c = (int32_t)(pn << (22 - 10 * k)) >> 22;
      packed_n[k] = (float)c / 511.0f;
    }
    for (int k = 0; k < 3; k++) {
      wp[k] = r[k][0] * p[0] + r[k][1] * p[1] + r[k][2] * p[2] + t[k];
      wn[k] = r[k][0] * packed_n[0] + r[k][1] * packed_n[1] + r[k][2] * packed_n[2];
    }
    tmuf_scenery_prelight(&b->scenery.view, flags, wp, wn, b->scenery.lamps, b->scenery.lamp_count, bgra + (size_t)i * 4u);
  }
  return m->vertex_count;
}

const tmuf_vehicle_visuals *tmuf_track_vehicle_visuals(const tmuf_track *track) {
  const tmuf_track_base *b = track ? tmuf_track_base_of(track) : NULL;
  return b && b->has_visuals ? &b->vehicle_visuals.view : NULL;
}

const tmuf_sound *tmuf_track_sound(const tmuf_track *track, const char *plain_path) {
  /* a cache the const track keeps (as its lazily built data) */
  tmuf_track_base *b = track ? (tmuf_track_base *)tmuf_track_base_of(track) : NULL;
  if (!b || !b->has_visuals || !plain_path)
    return NULL;
  for (uint32_t i = 0; i < b->sound_count; i++)
    if (strcmp(b->sounds[i].path, plain_path) == 0)
      return b->sounds[i].sound;
  tmuf_asset *a = tmuf_assets_load_path(&b->scene.assets, plain_path);
  const tmuf_sound *s =
      a && a->root && a->gbx.node_count ? tmuf_sound_build(&b->scene.assets, a, &a->gbx.nodes[0], &b->arena) : NULL;
  if (b->sound_count == b->sound_cap) {
    const uint32_t cap = b->sound_cap ? b->sound_cap * 2 : 16;
    struct tmuf_track_sound_entry *grown = realloc(b->sounds, sizeof *grown * cap);
    if (!grown)
      return s;
    b->sounds = grown;
    b->sound_cap = cap;
  }
  const size_t n = strlen(plain_path) + 1;
  char *path = tmuf_arena_alloc(&b->arena, n);
  if (!path)
    return s;
  memcpy(path, plain_path, n);
  b->sounds[b->sound_count++] = (struct tmuf_track_sound_entry){path, s};
  return s;
}

void tmuf_track_base_free(tmuf_track_base *b) {
  free(b->sounds);
  tmuf_visuals_free(&b->visuals);
  tmuf_vehicle_visuals_free(&b->vehicle_visuals);
  tmuf_lights_free(&b->lights);
  tmuf_lightmap_free(&b->lightmap);
  tmuf_weather_free(&b->weather);
  free(b->triangles);
  tmuf_scene_free(&b->scene);
  tmuf_arena_free(&b->arena);
  free(b->map_data);
  memset(b, 0, sizeof *b);
}

/* ---- zip archives (skins, lightmap caches) ---- */

static uint32_t zip_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t zip_u16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

static int zip_name_eq(const uint8_t *a, uint32_t n, const char *b) {
  if (strlen(b) != n)
    return 0;
  for (uint32_t i = 0; i < n; i++) {
    int x = a[i], y = (unsigned char)b[i];
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    if (x != y)
      return 0;
  }
  return 1;
}

void *tmuf_zip_extract(const void *zip, size_t size, const char *name, size_t *out_size) {
  const uint8_t *z = zip;
  if (!z || size < 22 || !name)
    return NULL;
  /* the end of central directory record, within its comment's reach */
  size_t eocd = size - 22;
  for (;; eocd--) {
    if (zip_u32(z + eocd) == 0x06054b50u)
      break;
    if (eocd == 0 || size - eocd > 22 + 65535)
      return NULL;
  }
  const uint32_t entries = zip_u16(z + eocd + 10), cd = zip_u32(z + eocd + 16);
  size_t at = cd;
  for (uint32_t i = 0; i < entries; i++) {
    if (at + 46 > size || zip_u32(z + at) != 0x02014b50u)
      return NULL;
    const uint32_t method = zip_u16(z + at + 10), csize = zip_u32(z + at + 20), usize = zip_u32(z + at + 24);
    const uint32_t nlen = zip_u16(z + at + 28), xlen = zip_u16(z + at + 30), clen = zip_u16(z + at + 32);
    const uint32_t local = zip_u32(z + at + 42);
    if (at + 46 + nlen > size)
      return NULL;
    if (zip_name_eq(z + at + 46, nlen, name)) {
      if ((size_t)local + 30 > size || zip_u32(z + local) != 0x04034b50u)
        return NULL;
      const size_t data = (size_t)local + 30 + zip_u16(z + local + 26) + zip_u16(z + local + 28);
      if (data + csize > size || (method != 0 && method != 8))
        return NULL;
      uint8_t *out = malloc(usize ? usize : 1);
      if (!out)
        return NULL;
      if (method == 0 ? (csize == usize && (memcpy(out, z + data, usize), 1))
                      : tmuf_inflate_raw(z + data, csize, out, usize)) {
        *out_size = usize;
        return out;
      }
      free(out);
      return NULL;
    }
    at += 46 + nlen + xlen + clen;
  }
  return NULL;
}

void tmuf_world_set_sound(tmuf_world *world, int on) {
  if (!world)
    return;
  world->sim.car.sound.enabled = on != 0;
  world->sim.car.sound.front_impact = world->sim.car.sound.rear_impact = world->sim.car.sound.body_impact = 0;
  world->sim.car.sound.checkpoint_corpus = -1;
}
