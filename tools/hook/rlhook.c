// rlhook.dll - offline reference hook for the real game (injected by rlinject.exe, EAC off, no network).
// Stage 1: locate GNames/GObjects by structure (no hardcoded addresses), auto-detect the reflection
// layout from known Core structs, dump the SDK, and detour the per-tick physics function.
// Build: x86_64-w64-mingw32-gcc -O1 -shared -o rlhook.dll rlhook.c -static-libgcc
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t i32;

static char g_out[MAX_PATH] = "C:\\rlhook_out";
static FILE *g_log;
static u8 *g_base;
static u32 g_img_size;
static u8 *g_text_lo, *g_text_hi;

static void logf_(const char *fmt, ...) {
  if (!g_log) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
}
#define LOG(...) logf_(__VA_ARGS__)

// ---- safe memory reads ----------------------------------------------------------------------
static int readable(const void *p, size_t n) {
  static u8 *lo, *hi; // cache of last good region
  const u8 *a = (const u8 *)p;
  if ((u64)a < 0x10000 || (u64)a > 0x7fffffffffffULL) return 0;
  if (a >= lo && a + n <= hi) return 1;
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(a, &mbi, sizeof mbi)) return 0;
  if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mbi.Protect & 0xEE)) return 0;
  u8 *rlo = (u8 *)mbi.BaseAddress, *rhi = rlo + mbi.RegionSize;
  if (a + n > rhi) return readable(rhi, (a + n) - rhi) && 1;
  lo = rlo;
  hi = rhi;
  return 1;
}
static void *rdp(const void *p) { return readable(p, 8) ? *(void **)p : NULL; }
static i32 rdi(const void *p) { return readable(p, 4) ? *(i32 *)p : (i32)0x80000000; }
static int in_image(const void *p) { return (u8 *)p >= g_base && (u8 *)p < g_base + g_img_size; }
static int in_text(const void *p) { return (u8 *)p >= g_text_lo && (u8 *)p < g_text_hi; }

// ---- reflection layout (all detected at runtime) ------------------------------------------------
static struct {
  void ***names;  // FNameEntry**
  i32 names_num;
  int name_str, name_wide;
  u8 **objs;
  i32 objs_num;
  u8 *names_arr, *objs_arr;
  int o_idx, o_name, o_class, o_outer;
  int f_next, s_super, s_children, s_size, p_offset, p_elsize, p_sub, fn_func, e_names, c_cdo;
} L;

static int name_get(i32 idx, char *out, int cap) {
  out[0] = 0;
  if (idx < 0 || idx >= L.names_num) return 0;
  u8 *e = (u8 *)rdp(&L.names[idx]);
  if (!e || !readable(e + L.name_str, 4)) return 0;
  int i = 0;
  if (L.name_wide) {
    WCHAR *w = (WCHAR *)(e + L.name_str);
    for (; i < cap - 1 && readable(w + i, 2) && w[i]; i++) out[i] = w[i] < 128 ? (char)w[i] : '?';
  } else {
    char *c = (char *)(e + L.name_str);
    for (; i < cap - 1 && readable(c + i, 1) && c[i]; i++) out[i] = c[i];
  }
  out[i] = 0;
  return i;
}
static i32 name_find(const char *s) {
  char b[256];
  for (i32 i = 0; i < L.names_num; i++)
    if (name_get(i, b, sizeof b) && !strcmp(b, s)) return i;
  return -1;
}
static u8 *obj_at(i32 i) { return (i >= 0 && i < L.objs_num) ? (u8 *)rdp(&L.objs[i]) : NULL; }
static i32 o_nameidx(u8 *o) { return rdi(o + L.o_name); }
static u8 *o_class(u8 *o) { return (u8 *)rdp(o + L.o_class); }
static u8 *o_outer(u8 *o) { return (u8 *)rdp(o + L.o_outer); }
static void o_name(u8 *o, char *out, int cap) {
  if (!o || !readable(o, 0x60)) {
    snprintf(out, cap, "<null>");
    return;
  }
  int n = name_get(o_nameidx(o), out, cap);
  i32 num = rdi(o + L.o_name + 4);
  if (num > 0 && num < 100000000) snprintf(out + n, cap - n, "_%d", num - 1);
}
static void o_full(u8 *o, char *out, int cap) {
  u8 *chain[32];
  int n = 0;
  for (u8 *p = o; p && n < 32 && readable(p, 0x60); p = o_outer(p)) chain[n++] = p;
  int pos = 0;
  out[0] = 0;
  for (int i = n - 1; i >= 0 && pos < cap - 2; i--) {
    char b[300];
    o_name(chain[i], b, sizeof b);
    pos += snprintf(out + pos, cap - pos, "%s%s", b, i ? "." : "");
  }
}

static int find_gnames(void) {
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
  IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
  for (int k = 0; k < nt->FileHeader.NumberOfSections; k++, s++) {
    if (memcmp(s->Name, ".data", 6)) continue;
    u8 *lo = g_base + s->VirtualAddress, *hi = lo + s->Misc.VirtualSize - 16;
    for (u8 *p = lo; p < hi; p += 8) {
      void **d = *(void ***)p;
      i32 num = *(i32 *)(p + 8), max = *(i32 *)(p + 12);
      if (num < 1000 || num > 20000000 || max < num || in_image(d) || !readable(d, 16)) continue;
      u8 *e0 = (u8 *)d[0], *e1 = (u8 *)d[1];
      if (!readable(e0, 0x60) || !readable(e1, 0x60)) continue;
      for (int off = 0; off <= 0x40; off += 4) {
        if (!memcmp(e0 + off, "None", 5) && !memcmp(e1 + off, "ByteProperty", 13)) {
          L.name_wide = 0, L.name_str = off;
        } else if (!memcmp(e0 + off, L"None", 10) && !memcmp(e1 + off, L"ByteProperty", 26)) {
          L.name_wide = 1, L.name_str = off;
        } else
          continue;
        L.names = (void ***)d, L.names_num = num, L.names_arr = p;
        return 1;
      }
    }
  }
  return 0;
}

static int find_gobjects(void) {
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
  IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
  for (int k = 0; k < nt->FileHeader.NumberOfSections; k++, s++) {
    if (memcmp(s->Name, ".data", 6)) continue;
    u8 *lo = g_base + s->VirtualAddress, *hi = lo + s->Misc.VirtualSize - 16;
    for (u8 *p = lo; p < hi; p += 8) {
      u8 **d = *(u8 ***)p;
      i32 num = *(i32 *)(p + 8), max = *(i32 *)(p + 12);
      if (p == L.names_arr || num < 5000 || num > 20000000 || max < num || in_image(d) || !readable(d, 8 * 64)) continue;
      for (int off = 0x20; off <= 0x60; off += 4) {
        int ok = 0, tot = 0;
        for (int i = 1; i < 64; i++) {
          u8 *o = d[i];
          if (!o) continue;
          tot++;
          if (readable(o, 0x80) && in_image(*(void **)o) && *(i32 *)(o + off) == i) ok++;
        }
        if (tot >= 32 && ok == tot) {
          L.objs = d, L.objs_num = num, L.objs_arr = p, L.o_idx = off;
          return 1;
        }
      }
    }
  }
  return 0;
}

static i32 N_Object, N_Class, N_Core;
static int find_uobject_layout(void) {
  N_Object = name_find("Object"), N_Class = name_find("Class"), N_Core = name_find("Core");
  if (N_Object < 0 || N_Class < 0 || N_Core < 0) return 0;
  for (int no = 0x20; no <= 0x78; no += 4)
    for (int co = 0x20; co <= 0x78; co += 8) {
      if (co == no || co == no - 4) continue;
      for (i32 i = 0; i < L.objs_num && i < 20000; i++) {
        u8 *o = obj_at(i);
        if (!o || !readable(o, 0x80) || *(i32 *)(o + no) != N_Object) continue;
        u8 *c = *(u8 **)(o + co);
        if (!readable(c, 0x80) || *(i32 *)(c + no) != N_Class || *(u8 **)(c + co) != c) continue;
        for (int oo = 0x20; oo <= 0x78; oo += 8) {
          u8 *ou = *(u8 **)(o + oo);
          if (oo != co && readable(ou, 0x80) && *(i32 *)(ou + no) == N_Core) {
            L.o_name = no, L.o_class = co, L.o_outer = oo;
            return 1;
          }
        }
      }
    }
  return 0;
}

// find object by class name / name / outer (outer NULL = any)
static u8 *find_obj(const char *cls, const char *name, u8 *outer) {
  i32 nc = name_find(cls), nn = name_find(name);
  if (nc < 0 || nn < 0) return NULL;
  for (i32 i = 0; i < L.objs_num; i++) {
    u8 *o = obj_at(i);
    if (!o || !readable(o, 0x80) || o_nameidx(o) != nn) continue;
    u8 *c = o_class(o);
    if (!c || !readable(c, 0x80) || o_nameidx(c) != nc) continue;
    if (outer && o_outer(o) != outer) continue;
    return o;
  }
  return NULL;
}

static int detect_reflection(void) {
  u8 *core = find_obj("Package", "Core", NULL);
  u8 *cObject = find_obj("Class", "Object", core);
  u8 *sVector = find_obj("ScriptStruct", "Vector", cObject);
  u8 *sPlane = find_obj("ScriptStruct", "Plane", cObject);
  u8 *sBox = find_obj("ScriptStruct", "Box", cObject);
  u8 *sColor = find_obj("ScriptStruct", "Color", cObject);
  if (!sVector || !sPlane || !sBox || !sColor) {
    LOG("anchors missing: core=%p Object=%p Vector=%p Plane=%p Box=%p Color=%p", core, cObject, sVector, sPlane, sBox, sColor);
    return 0;
  }
  u8 *vx = find_obj("FloatProperty", "X", sVector), *vy = find_obj("FloatProperty", "Y", sVector), *vz = find_obj("FloatProperty", "Z", sVector);
  u8 *pw = find_obj("FloatProperty", "W", sPlane);
  u8 *bmin = find_obj("StructProperty", "Min", sBox), *bmax = find_obj("StructProperty", "Max", sBox);
  if (!vx || !vy || !vz || !pw || !bmin || !bmax) {
    LOG("prop anchors missing %p %p %p %p %p %p", vx, vy, vz, pw, bmin, bmax);
    return 0;
  }
  L.p_offset = L.p_elsize = L.f_next = L.s_children = L.s_super = L.s_size = L.p_sub = -1;
  for (int o = 0x40; o < 0x140; o += 4) {
    if (L.p_offset < 0 && rdi(vx + o) == 0 && rdi(vy + o) == 4 && rdi(vz + o) == 8 && rdi(pw + o) == 12 && rdi(bmax + o) == 12) L.p_offset = o;
    if (L.p_elsize < 0 && rdi(vx + o) == 4 && rdi(vz + o) == 4 && rdi(bmin + o) == 12 && rdi(bmax + o) == 12 && rdi(vx + o - 4) == 1) L.p_elsize = o;
    if (L.s_size < 0 && rdi(sVector + o) == 12 && rdi(sPlane + o) == 16 && rdi(sColor + o) == 4) L.s_size = o;
  }
  for (int o = 0x40; o < 0x140; o += 8) {
    u8 *n = (u8 *)rdp(vy + o);
    if (L.f_next < 0 && (n == vx || n == vz)) {
      u8 *a = (u8 *)rdp(vx + o), *b = (u8 *)rdp(vz + o);
      if ((a == vy || a == NULL) && (b == vy || b == NULL)) L.f_next = o;
    }
    u8 *c = (u8 *)rdp(sVector + o);
    if (L.s_children < 0 && (c == vx || c == vy || c == vz)) L.s_children = o;
    if (L.s_super < 0 && rdp(sPlane + o) == sVector && rdp(sVector + o) == NULL) L.s_super = o;
    if (L.p_sub < 0 && rdp(bmin + o) == sVector && rdp(bmax + o) == sVector) L.p_sub = o;
  }
  // UFunction::Func - offset where most Function objects hold a .text pointer
  L.fn_func = -1;
  {
    i32 nf = name_find("Function");
    int votes[0x40] = {0}, seen = 0;
    for (i32 i = 0; i < L.objs_num && seen < 3000; i++) {
      u8 *o = obj_at(i);
      if (!o || !readable(o, 0x200)) continue;
      u8 *c = o_class(o);
      if (!c || !readable(c, 0x80) || o_nameidx(c) != nf) continue;
      seen++;
      for (int k = 0; k < 0x40; k++)
        if (in_text(*(void **)(o + 0x60 + k * 8))) votes[k]++;
    }
    int best = 0;
    for (int k = 1; k < 0x40; k++)
      if (votes[k] > votes[best]) best = k;
    if (votes[best] > seen / 2) L.fn_func = 0x60 + best * 8;
    LOG("Function objects sampled=%d func_votes=%d", seen, votes[best]);
  }
  // UClass::ClassDefaultObject - pointer whose target's class is the class itself
  L.c_cdo = -1;
  {
    u8 *cActor = find_obj("Class", "Actor", NULL);
    for (int o = 0x80; o < 0x400 && cObject && cActor; o += 8) {
      u8 *a = (u8 *)rdp(cObject + o), *b = (u8 *)rdp(cActor + o);
      if (a && b && readable(a, 0x80) && readable(b, 0x80) && o_class(a) == cObject && o_class(b) == cActor) {
        L.c_cdo = o;
        break;
      }
    }
  }
  L.e_names = L.f_next >= 0 ? L.f_next + 8 : -1;
  return L.p_offset >= 0 && L.f_next >= 0 && L.s_children >= 0 && L.s_super >= 0 && L.s_size >= 0;
}

// ---- SDK dump ------------------------------------------------------------------------------
static int is_a(u8 *cls, i32 name_idx) { // cls is a UClass; walk supers
  for (int g = 0; cls && g < 64 && readable(cls, 0x100); g++, cls = (u8 *)rdp(cls + L.s_super))
    if (o_nameidx(cls) == name_idx) return 1;
  return 0;
}

static void dump_sdk(void) {
  char path[MAX_PATH], nm[300], full[1024], cn[300], b2[1024];
  snprintf(path, sizeof path, "%s\\names.txt", g_out);
  FILE *f = fopen(path, "wb");
  if (f) {
    for (i32 i = 0; i < L.names_num; i++)
      if (name_get(i, nm, sizeof nm)) fprintf(f, "%d\t%s\n", i, nm);
    fclose(f);
  }
  snprintf(path, sizeof path, "%s\\objects.txt", g_out);
  f = fopen(path, "wb");
  if (f) {
    for (i32 i = 0; i < L.objs_num; i++) {
      u8 *o = obj_at(i);
      if (!o || !readable(o, 0x60)) continue;
      o_full(o, full, sizeof full);
      o_name(o_class(o), cn, sizeof cn);
      fprintf(f, "%d\t%p\t%llx\t%s\t%s\n", i, o, (unsigned long long)((u8 *)rdp(o) - g_base), cn, full);
    }
    fclose(f);
  }
  i32 nStruct = name_find("Struct"), nClass = name_find("Class"), nProperty = name_find("Property"), nFunction = name_find("Function"),
      nEnum = name_find("Enum"), nConst = name_find("Const"), nState = name_find("State");
  snprintf(path, sizeof path, "%s\\sdk.txt", g_out);
  f = fopen(path, "wb");
  snprintf(path, sizeof path, "%s\\defaults.bin", g_out);
  FILE *fd = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "# RLiOS SDK dump. Lines: S=struct/class  P=property  F=function  E=enum  C=const  D=defaults(blob offset,size in defaults.bin)\n");
  int ns = 0, np = 0, nf = 0;
  for (i32 i = 0; i < L.objs_num; i++) {
    u8 *o = obj_at(i);
    if (!o || !readable(o, 0x100)) continue;
    u8 *mc = o_class(o);
    if (!mc) continue;
    if (is_a(mc, nEnum)) {
      o_full(o, full, sizeof full);
      fprintf(f, "E\t%s", full);
      u8 *d = (u8 *)rdp(o + L.e_names);
      i32 n = rdi(o + L.e_names + 8);
      for (i32 k = 0; d && k < n && k < 1024 && readable(d + k * 8, 8); k++) {
        name_get(*(i32 *)(d + k * 8), nm, sizeof nm);
        fprintf(f, "\t%s", nm);
      }
      fputc('\n', f);
      continue;
    }
    if (is_a(mc, nConst)) {
      o_full(o, full, sizeof full);
      WCHAR *w = (WCHAR *)rdp(o + L.f_next + 8);
      i32 n = rdi(o + L.f_next + 16);
      int p = 0;
      for (i32 k = 0; w && k < n && k < 500 && readable(w + k, 2) && w[k]; k++) b2[p++] = w[k] < 128 ? (char)w[k] : '?';
      b2[p] = 0;
      fprintf(f, "C\t%s\t%s\n", full, b2);
      continue;
    }
    if (!is_a(mc, nStruct)) continue;
    // struct, class or state
    o_full(o, full, sizeof full);
    o_name(mc, cn, sizeof cn);
    u8 *sup = (u8 *)rdp(o + L.s_super);
    b2[0] = 0;
    if (sup) o_full(sup, b2, sizeof b2);
    i32 size = rdi(o + L.s_size);
    fprintf(f, "S\t%s\t%s\t%s\t0x%x\t%p\n", cn, full, b2, size, o);
    ns++;
    for (u8 *c = (u8 *)rdp(o + L.s_children); c && readable(c, 0x100); c = (u8 *)rdp(c + L.f_next)) {
      u8 *cc = o_class(c);
      if (!cc) break;
      o_name(c, nm, sizeof nm);
      o_name(cc, cn, sizeof cn);
      if (is_a(cc, nProperty)) {
        char sub[1024] = "";
        if (L.p_sub >= 0) {
          u8 *s1 = (u8 *)rdp(c + L.p_sub), *s2 = (u8 *)rdp(c + L.p_sub + 8);
          if (!strcmp(cn, "BoolProperty"))
            snprintf(sub, sizeof sub, "mask=0x%x", (u32)rdi(c + L.p_sub));
          else if (s1 && !in_image(s1) && readable(s1, 0x80) && rdi(s1 + L.o_idx) >= 0 && obj_at(rdi(s1 + L.o_idx)) == s1) {
            o_full(s1, sub, sizeof sub);
            if ((!strcmp(cn, "ClassProperty") || !strcmp(cn, "MapProperty")) && s2 && readable(s2, 0x80) && obj_at(rdi(s2 + L.o_idx)) == s2) {
              int l = strlen(sub);
              sub[l++] = '|';
              o_full(s2, sub + l, sizeof sub - l);
            }
            if (!strcmp(cn, "ArrayProperty")) { // inner property: report its class and its own sub-object
              char ic[300], isub[600] = "";
              o_name(o_class(s1), ic, sizeof ic);
              u8 *is = (u8 *)rdp(s1 + L.p_sub);
              if (is && !in_image(is) && readable(is, 0x80) && obj_at(rdi(is + L.o_idx)) == is) o_full(is, isub, sizeof isub);
              snprintf(sub, sizeof sub, "%s<%s>", ic, isub);
            }
          }
        }
        fprintf(f, "P\t%s\t%s\t0x%x\t0x%x\t%d\t0x%llx\t%s\n", nm, cn, rdi(c + L.p_offset), rdi(c + L.p_elsize), rdi(c + L.p_elsize - 4),
                (unsigned long long)(readable(c + L.p_elsize + 4, 8) ? *(u64 *)(c + L.p_elsize + 4) : 0), sub);
        np++;
      } else if (is_a(cc, nFunction)) {
        u8 *fn = L.fn_func >= 0 ? (u8 *)rdp(c + L.fn_func) : NULL;
        fprintf(f, "F\t%s\t%llx\t%p\n", nm, (unsigned long long)(fn && in_text(fn) ? fn - g_base : 0), c);
        nf++;
      }
    }
    if (fd && L.c_cdo >= 0 && o_nameidx(mc) == nClass) {
      u8 *cdo = (u8 *)rdp(o + L.c_cdo);
      if (cdo && size > 0 && size < 0x100000 && readable(cdo, size)) {
        long at = ftell(fd);
        fwrite(cdo, 1, size, fd);
        fprintf(f, "D\t%ld\t%d\t%p\t%llx\n", at, size, cdo, (unsigned long long)((u8 *)rdp(cdo) - g_base));
      }
    }
  }
  fclose(f);
  if (fd) fclose(fd);
  LOG("sdk dumped: %d structs/classes, %d properties, %d functions", ns, np, nf);
}


static volatile LONG g_tick_count;
// ---- command channel (runs on the game thread, from the tick detour) --------------------------
// <out>\cmd.txt, one command per line; results appended to <out>\cmd.out
//   find <ClassName>                      list live instances (IsA), non-default
//   hex <ptr-hex> <len>                   hex dump memory
//   call <target> <Func> [params...]      ProcessEvent. target = full object name, or @ClassName (first live instance), or ptr-hex with 0x
//        params: i:<int> f:<float> x:<hexbytes> z:<nzero> s:<text> (FString) n:<Name> (FName) o:<full object name> p:<ptr-hex>
static u8 *find_full(const char *full) {
  char b[1024];
  for (i32 i = 0; i < L.objs_num; i++) {
    u8 *o = obj_at(i);
    if (!o || !readable(o, 0x60)) continue;
    o_full(o, b, sizeof b);
    if (!strcmp(b, full)) return o;
  }
  return NULL;
}
static u8 *find_instance(const char *cls, int nth, FILE *list) {
  i32 nc = name_find(cls);
  char b[1024];
  if (nc < 0) return NULL;
  for (i32 i = 0; i < L.objs_num; i++) {
    u8 *o = obj_at(i);
    if (!o || !readable(o, 0x60)) continue;
    u8 *c = o_class(o);
    if (!c || !is_a(c, nc)) continue;
    name_get(o_nameidx(o), b, sizeof b);
    if (!strncmp(b, "Default__", 9)) continue;
    if (list) {
      o_full(o, b, sizeof b);
      fprintf(list, "  %p %s\n", o, b);
    } else if (nth-- == 0)
      return o;
  }
  return NULL;
}
static u8 *find_function(u8 *obj, const char *fn) {
  i32 nf = name_find(fn), nFunction = name_find("Function");
  for (u8 *c = o_class(obj); c && readable(c, 0x100); c = (u8 *)rdp(c + L.s_super))
    for (u8 *k = (u8 *)rdp(c + L.s_children); k && readable(k, 0x100); k = (u8 *)rdp(k + L.f_next))
      if (o_nameidx(k) == nf && o_class(k) && is_a(o_class(k), nFunction)) return k;
  return NULL;
}
static u8 *resolve_target(char *t) {
  if (t[0] == '0' && t[1] == 'x') return (u8 *)strtoull(t, NULL, 16);
  if (t[0] == '@') return find_instance(t + 1, 0, NULL);
  return find_full(t);
}
static int add_watch(char **tok, int nt, int isp);
static void clear_watch(void);
static FILE *g_rec;
static int g_rec_left;
static char g_rec_name[128];
static WCHAR g_strpool[16][512];
static void run_commands(void) {
  char path[MAX_PATH], path2[MAX_PATH], line[4096];
  snprintf(path, sizeof path, "%s\\cmd.txt", g_out);
  if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;
  snprintf(path2, sizeof path2, "%s\\cmd.run", g_out);
  DeleteFileA(path2);
  if (!MoveFileA(path, path2)) return;
  FILE *in = fopen(path2, "rb");
  snprintf(path, sizeof path, "%s\\cmd.out", g_out);
  FILE *out = fopen(path, "ab");
  if (!in || !out) return;
  L.names = *(void ****)L.names_arr, L.names_num = *(i32 *)(L.names_arr + 8);
  L.objs = *(u8 ***)L.objs_arr, L.objs_num = *(i32 *)(L.objs_arr + 8);
  while (fgets(line, sizeof line, in)) {
    line[strcspn(line, "\r\n")] = 0;
    if (!line[0]) continue;
    fprintf(out, "> %s  [tick %ld]\n", line, g_tick_count);
    char *tok[40];
    int nt = 0;
    for (char *p = strtok(line, " "); p && nt < 40; p = strtok(NULL, " ")) tok[nt++] = p;
    if (!nt) continue;
    if (!strcmp(tok[0], "find") && nt >= 2) {
      find_instance(tok[1], 0, out);
    } else if (!strcmp(tok[0], "hex") && nt >= 3) {
      u8 *p = (u8 *)strtoull(tok[1], NULL, 16);
      int n = atoi(tok[2]);
      for (int i = 0; i < n; i += 16) {
        if (!readable(p + i, 16)) {
          fprintf(out, "  unreadable\n");
          break;
        }
        fprintf(out, "  %04x:", i);
        for (int k = 0; k < 16; k++) fprintf(out, " %02x", p[i + k]);
        fputc('\n', out);
      }
    } else if (!strcmp(tok[0], "watch") && nt >= 5) {
      fprintf(out, "  %s\n", add_watch(tok, nt, 0) ? "ok" : "target not found");
    } else if (!strcmp(tok[0], "watchp") && nt >= 6) {
      fprintf(out, "  %s\n", add_watch(tok, nt, 1) ? "ok" : "target not found");
    } else if (!strcmp(tok[0], "recstart") && nt >= 3) {
      char rp[MAX_PATH];
      snprintf(g_rec_name, sizeof g_rec_name, "%s", tok[1]);
      snprintf(rp, sizeof rp, "%s\\%s", g_out, tok[1]);
      if (g_rec) fclose(g_rec);
      g_rec = fopen(rp, "wb");
      g_rec_left = atoi(tok[2]);
      fprintf(out, "  recording %d ticks -> %s (%s)\n", g_rec_left, rp, g_rec ? "ok" : "open failed");
    } else if (!strcmp(tok[0], "unwatch")) {
      clear_watch();
    } else if (!strcmp(tok[0], "poke") && nt >= 4) {
      u8 *o = resolve_target(tok[1]);
      int off = (int)strtol(tok[2], NULL, 0);
      char *v = tok[3];
      if (o && readable(o + off, strlen(v) / 2))
        for (; v[0] && v[1]; v += 2) {
          char h[3] = {v[0], v[1], 0};
          o[off++] = (u8)strtoul(h, NULL, 16);
        }
      fprintf(out, "  poke %p %s\n", o, o ? "ok" : "target not found");
    } else if (!strcmp(tok[0], "call") && nt >= 3) {
      u8 *obj = resolve_target(tok[1]);
      u8 *fn = obj && readable(obj, 0x60) ? find_function(obj, tok[2]) : NULL;
      if (!obj || !fn) {
        fprintf(out, "  target=%p function=%p : not found\n", obj, fn);
        continue;
      }
      static u8 parms[4096];
      memset(parms, 0, sizeof parms);
      int pos = 0, sp = 0;
      for (int i = 3; i < nt; i++) {
        char *v = tok[i] + 2;
        switch (tok[i][0]) {
        case 'i': *(i32 *)(parms + pos) = (i32)strtoll(v, NULL, 0), pos += 4; break;
        case 'f': *(float *)(parms + pos) = (float)atof(v), pos += 4; break;
        case 'z': pos += atoi(v); break;
        case 'p': *(u64 *)(parms + pos) = strtoull(v, NULL, 16), pos += 8; break;
        case 'o': *(u8 **)(parms + pos) = find_full(v), pos += 8; break;
        case 'n': *(i32 *)(parms + pos) = name_find(v), pos += 8; break;
        case 'x':
          for (; v[0] && v[1]; v += 2) {
            char h[3] = {v[0], v[1], 0};
            parms[pos++] = (u8)strtoul(h, NULL, 16);
          }
          break;
        case 's': {
          WCHAR *w = g_strpool[sp++ & 15];
          int n = 0;
          for (; v[n] && n < 510; n++) w[n] = (WCHAR)(v[n] == '+' ? ' ' : v[n]);
          w[n] = 0;
          *(WCHAR **)(parms + pos) = w, *(i32 *)(parms + pos + 8) = n + 1, *(i32 *)(parms + pos + 12) = n + 1, pos += 16;
        } break;
        }
      }
      char fnm[1024];
      o_full(fn, fnm, sizeof fnm);
      fprintf(out, "  ProcessEvent(%p, %s, %d bytes)\n", obj, fnm, pos);
      fflush(out);
      typedef void (*pe_fn)(void *, void *, void *, void *);
      ((pe_fn)(*(void ***)obj)[67])(obj, fn, parms, NULL);
      fprintf(out, "  returned; parms after:");
      for (int k = 0; k < (pos + 32 < 128 ? pos + 32 : 128); k++) fprintf(out, " %02x", parms[k]);
      fputc('\n', out);
    }
    fflush(out);
  }
  fclose(in);
  fclose(out);
  DeleteFileA(path2);
}
static volatile LONG g_ready;

// ---- per-tick watches: raw bytes of chosen memory ranges, logged at the end of every physics tick ----
//   watch <label> <target> <off> <len>            bytes at target+off
//   watchp <label> <target> <ptroff> <off> <len>  bytes at (*(target+ptroff))+off, pointer re-read each tick
//   unwatch                                       clear all
//   poke <target> <off> <hexbytes>                write memory (scenario setup)
static struct { char label[32]; u8 *base; int ptroff, off, len; } g_watch[32];
static int g_nwatch;
//   recstart <file> <nticks>                    log all watches for exactly n ticks into <out>\<file>, then write <file>.done and clear
static void record_watches(void *self) {
  if (!g_nwatch || !g_rec) return;
  i32 frame = *(i32 *)((u8 *)self + 0x1e8);
  for (int i = 0; i < g_nwatch; i++) {
    u8 *p = g_watch[i].base;
    if (g_watch[i].ptroff >= 0) p = (u8 *)rdp(p + g_watch[i].ptroff);
    fprintf(g_rec, "%ld\t%d\t%s\t", g_tick_count, frame, g_watch[i].label);
    if (p && readable(p + g_watch[i].off, g_watch[i].len))
      for (int k = 0; k < g_watch[i].len; k++) fprintf(g_rec, "%02x", p[g_watch[i].off + k]);
    else
      fprintf(g_rec, "UNREADABLE");
    fputc('\n', g_rec);
  }
  if (--g_rec_left <= 0) {
    char path[MAX_PATH];
    fclose(g_rec);
    g_rec = NULL, g_nwatch = 0;
    snprintf(path, sizeof path, "%s\\%s.done", g_out, g_rec_name);
    FILE *d = fopen(path, "wb");
    if (d) fclose(d);
  }
}

static int add_watch(char **tok, int nt, int isp) {
  u8 *o = resolve_target(tok[2]);
  if (!o || g_nwatch >= 32) return 0;
  snprintf(g_watch[g_nwatch].label, 32, "%s", tok[1]);
  g_watch[g_nwatch].base = o;
  g_watch[g_nwatch].ptroff = isp ? (int)strtol(tok[3], NULL, 0) : -1;
  g_watch[g_nwatch].off = (int)strtol(tok[3 + isp], NULL, 0);
  g_watch[g_nwatch].len = (int)strtol(tok[4 + isp], NULL, 0);
  g_nwatch++;
  return 1;
}
static void clear_watch(void) {
  g_nwatch = 0;
  if (g_rec) fclose(g_rec), g_rec = NULL;
}

// ---- physics tick detour ---------------------------------------------------------------------
// Target: per-tick physics function (see work notes): `or dword [rcx+0x1b8],8` right after the
// prologue, PhysicsFrame counter at this+0x1e8. Found by byte signature; args: rcx=this, xmm1=dt, xmm2=gravityZ.
typedef void (*tick_fn)(void *self, float dt, float gz);
static tick_fn g_tick_orig;
static FILE *g_ticks;
static void tick_detour(void *self, float dt, float gz) {
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  i32 frame_before = *(i32 *)((u8 *)self + 0x1e8);
  if (g_ready && (g_tick_count % 30) == 0) run_commands();
  g_tick_orig(self, dt, gz);
  LONG n = InterlockedIncrement(&g_tick_count);
  record_watches(self);
  if (g_ticks && n <= 2000000) {
    u32 dtb, gzb;
    memcpy(&dtb, &dt, 4);
    memcpy(&gzb, &gz, 4);
    fprintf(g_ticks, "%ld\t%p\t%d\t%08x\t%08x\t%lld\t%lu\n", n, self, frame_before, dtb, gzb, (long long)q.QuadPart, GetCurrentThreadId());
    if ((n & 0x7f) == 0) fflush(g_ticks);
  }
}

static u8 *sig_scan(const u8 *pat, const char *mask, size_t n) {
  for (u8 *p = g_text_lo; p + n < g_text_hi; p++) {
    size_t i = 0;
    for (; i < n; i++)
      if (mask[i] == 'x' && p[i] != pat[i]) break;
    if (i == n) return p;
  }
  return NULL;
}

static int hook_tick(void) {
  // 48 89 5C 24 08  48 89 6C 24 18  48 89 74 24 20  57  41 56  41 57  48 83 EC 50
  // 0F 29 74 24 40  0F 29 7C 24 30  0F 28 FA  0F 28 F1  48 8B D9  83 89 B8 01 00 00 08
  static const u8 pat[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41,
                           0x57, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0x29, 0x74, 0x24, 0x40, 0x0F, 0x29, 0x7C, 0x24, 0x30, 0x0F, 0x28, 0xFA, 0x0F,
                           0x28, 0xF1, 0x48, 0x8B, 0xD9, 0x83, 0x89, 0xB8, 0x01, 0x00, 0x00, 0x08};
  static const char mask[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????x";
  u8 *t = sig_scan(pat, mask, sizeof pat);
  if (!t) {
    LOG("tick signature not found");
    return 0;
  }
  LOG("physics tick function at %p (rva %llx)", t, (unsigned long long)(t - g_base));
  // trampoline: first 15 bytes (three position-independent mov [rsp+x],reg) + jmp back
  u8 *tr = (u8 *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (!tr) return 0;
  memcpy(tr, t, 15);
  tr[15] = 0xFF, tr[16] = 0x25, *(u32 *)(tr + 17) = 0;
  *(u64 *)(tr + 21) = (u64)(t + 15);
  g_tick_orig = (tick_fn)tr;
  u8 patch[15] = {0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90};
  *(u64 *)(patch + 6) = (u64)tick_detour;
  DWORD old;
  VirtualProtect(t, 15, PAGE_EXECUTE_READWRITE, &old);
  memcpy(t, patch, 15);
  VirtualProtect(t, 15, old, &old);
  FlushInstructionCache(GetCurrentProcess(), t, 15);
  return 1;
}


// ---- AES key capture: detour AESDecrypt(data, size, key256) (RVA 0x2DB210 in build 2026-10-31, found by signature) ----
typedef void (*aes_fn)(u8 *data, u32 size, const u8 *key);
static aes_fn g_aes_orig;
static u8 g_keys[16][32];
static int g_nkeys;
static CRITICAL_SECTION g_aes_cs;
static void aes_detour(u8 *data, u32 size, const u8 *key) {
  u8 first[16] = {0};
  if (size >= 16) memcpy(first, data, 16);
  g_aes_orig(data, size, key);
  EnterCriticalSection(&g_aes_cs);
  int known = 0;
  for (int i = 0; i < g_nkeys; i++)
    if (!memcmp(g_keys[i], key, 32)) known = 1;
  static int logged;
  if (!known || logged < 64) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\aes_keys.txt", g_out);
    FILE *f = fopen(path, "ab");
    if (f) {
      fprintf(f, "%s key=", known ? "call" : "NEWKEY");
      for (int k = 0; k < 32; k++) fprintf(f, "%02x", key[k]);
      fprintf(f, " size=%u in=", size);
      for (int k = 0; k < 16; k++) fprintf(f, "%02x", first[k]);
      fprintf(f, " out=");
      for (int k = 0; k < 16 && k < (int)size; k++) fprintf(f, "%02x", data[k]);
      fputc('\n', f);
      fclose(f);
    }
    logged++;
    if (!known && g_nkeys < 16) memcpy(g_keys[g_nkeys++], key, 32);
  }
  LeaveCriticalSection(&g_aes_cs);
}
static int hook_aes(void) {
  // 48 89 5C 24 10  55 56 57  48 81 EC 20 01 00 00  48 8B 05 ?? ?? ?? ??  48 33 C4 ... 49 8B D8 8B FA 48 8B F1
  static const u8 pat[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x05, 0, 0, 0, 0,
                           0x48, 0x33, 0xC4, 0x48, 0x89, 0x84, 0x24, 0x10, 0x01, 0x00, 0x00, 0x49, 0x8B, 0xD8, 0x8B, 0xFA, 0x48, 0x8B, 0xF1};
  static const char mask[] = "xxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxx";
  u8 *t = sig_scan(pat, mask, sizeof pat);
  if (!t) {
    LOG("AESDecrypt signature not found");
    return 0;
  }
  InitializeCriticalSection(&g_aes_cs);
  u8 *tr = (u8 *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  memcpy(tr, t, 15);
  tr[15] = 0xFF, tr[16] = 0x25, *(u32 *)(tr + 17) = 0;
  *(u64 *)(tr + 21) = (u64)(t + 15);
  g_aes_orig = (aes_fn)tr;
  u8 patch[15] = {0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90};
  *(u64 *)(patch + 6) = (u64)aes_detour;
  DWORD old;
  VirtualProtect(t, 15, PAGE_EXECUTE_READWRITE, &old);
  memcpy(t, patch, 15);
  VirtualProtect(t, 15, old, &old);
  FlushInstructionCache(GetCurrentProcess(), t, 15);
  LOG("AESDecrypt at %p (rva %llx) hooked", t, (unsigned long long)(t - g_base));
  return 1;
}

static void log_module(const char *name) {
  HMODULE m = GetModuleHandleA(name);
  if (!m) {
    LOG("module %s: not loaded", name);
    return;
  }
  char p[MAX_PATH] = "";
  GetModuleFileNameA(m, p, sizeof p);
  int builtin = readable((u8 *)m + 0x40, 16) && !memcmp((u8 *)m + 0x40, "Wine builtin DLL", 16);
  LOG("module %s: %p %s [%s]", name, m, p, builtin ? "WINE BUILTIN" : "native PE");
}

static DWORD WINAPI main_thread(LPVOID arg) {
  (void)arg;
  char path[MAX_PATH];
  GetEnvironmentVariableA("RLHOOK_OUT", g_out, sizeof g_out);
  CreateDirectoryA(g_out, NULL);
  snprintf(path, sizeof path, "%s\\hook.log", g_out);
  g_log = fopen(path, "wb");
  g_base = (u8 *)GetModuleHandleA(NULL);
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
  g_img_size = nt->OptionalHeader.SizeOfImage;
  IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
  g_text_lo = g_base + s->VirtualAddress, g_text_hi = g_text_lo + s->Misc.VirtualSize;
  LOG("rlhook attached. base=%p size=%x timestamp=%08x", g_base, g_img_size, (u32)nt->FileHeader.TimeDateStamp);

  snprintf(path, sizeof path, "%s\\ticks.tsv", g_out);
  g_ticks = fopen(path, "wb");
  if (g_ticks) fprintf(g_ticks, "#n\tself\tphysics_frame_before\tdt_bits\tgravz_bits\tqpc\tthread\n");
  hook_tick();
  hook_aes();

  int ok = 0;
  for (int i = 0; i < 1200 && !ok; i++) {
    Sleep(250);
    ok = find_gnames() && find_gobjects();
  }
  if (!ok) {
    LOG("GNames/GObjects not found (names=%p objs=%p)", L.names, L.objs);
    return 0;
  }
  LOG("GNames array @rva %llx data=%p num=%d str_off=0x%x wide=%d", (unsigned long long)(L.names_arr - g_base), L.names, L.names_num, L.name_str, L.name_wide);
  LOG("GObjects array @rva %llx data=%p num=%d index_off=0x%x", (unsigned long long)(L.objs_arr - g_base), L.objs, L.objs_num, L.o_idx);
  char dl[32] = "45";
  GetEnvironmentVariableA("RLHOOK_DUMP_DELAY", dl, sizeof dl);
  Sleep(atoi(dl) * 1000);
  // arrays may have been reallocated while loading
  L.names = *(void ****)L.names_arr, L.names_num = *(i32 *)(L.names_arr + 8);
  L.objs = *(u8 ***)L.objs_arr, L.objs_num = *(i32 *)(L.objs_arr + 8);
  LOG("at dump time: names=%d objects=%d ticks so far=%ld", L.names_num, L.objs_num, g_tick_count);
  log_module("ucrtbase.dll");
  log_module("api-ms-win-crt-math-l1-1-0.dll");
  log_module("msvcp140.dll");
  log_module("vcruntime140.dll");
  if (!find_uobject_layout()) {
    LOG("UObject layout not found");
    return 0;
  }
  LOG("UObject: name=0x%x class=0x%x outer=0x%x", L.o_name, L.o_class, L.o_outer);
  int r = detect_reflection();
  LOG("reflection(%d): UField.Next=0x%x UStruct.Super=0x%x Children=0x%x PropertySize=0x%x UProperty.Offset=0x%x ElementSize=0x%x sub=0x%x "
      "UFunction.Func=0x%x UClass.CDO=0x%x",
      r, L.f_next, L.s_super, L.s_children, L.s_size, L.p_offset, L.p_elsize, L.p_sub, L.fn_func, L.c_cdo);
  if (r) dump_sdk();
  g_ready = r;
  snprintf(path, sizeof path, "%s\\DONE", g_out);
  FILE *f = fopen(path, "wb");
  if (f) fprintf(f, "%d\n", r), fclose(f);
  if (GetEnvironmentVariableA("RLHOOK_EXIT_AFTER_DUMP", dl, sizeof dl) && dl[0] == '1') {
    LOG("exiting game after dump (ticks=%ld)", g_tick_count);
    if (g_ticks) fflush(g_ticks);
    TerminateProcess(GetCurrentProcess(), 0);
  }
  return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
  (void)r;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(h);
    CreateThread(NULL, 0, main_thread, NULL, 0, NULL);
  }
  return TRUE;
}
