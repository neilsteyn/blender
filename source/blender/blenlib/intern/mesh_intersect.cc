/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <fstream>
#include <iostream>

#include "BLI_allocator.hh"
#include "BLI_array.hh"
#include "BLI_assert.h"
#include "BLI_delaunay_2d.h"
#include "BLI_double3.hh"
#include "BLI_float3.hh"
#include "BLI_hash.hh"
#include "BLI_kdopbvh.h"
#include "BLI_map.hh"
#include "BLI_math_mpq.hh"
#include "BLI_mpq2.hh"
#include "BLI_mpq3.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "BLI_mesh_intersect.hh"

// #define PERFDEBUG

namespace blender::meshintersect {

#ifdef PERFDEBUG
static void perfdata_init(void);
static void incperfcount(int countnum);
static void doperfmax(int maxnum, int val);
static void dump_perfdata(void);
#endif

Vert::Vert(const mpq3 &mco, const double3 &dco, int id, int orig)
    : co_exact(mco), co(dco), id(id), orig(orig)
{
}

Vert::Vert(const Vert &other)
    : co_exact(other.co_exact), co(other.co), id(other.id), orig(other.orig)
{
}

Vert::Vert(Vert &&other) noexcept
    : co_exact(std::move(other.co_exact)), co(std::move(other.co)), id(other.id), orig(other.orig)
{
}

Vert &Vert::operator=(const Vert &other)
{
  if (this != &other) {
    this->co_exact = other.co_exact;
    this->co = other.co;
    this->id = other.id;
    this->orig = other.orig;
  }
  return *this;
}

Vert &Vert::operator=(Vert &&other) noexcept
{
  this->co_exact = std::move(other.co_exact);
  this->co = std::move(other.co);
  this->id = other.id;
  this->orig = other.orig;
  return *this;
}

bool Vert::operator==(const Vert &other) const
{
  return this->co_exact == other.co_exact;
}

uint32_t Vert::hash() const
{
  return co_exact.hash();
}

std::ostream &operator<<(std::ostream &os, Vertp v)
{
  os << "v" << v->id;
  if (v->orig != NO_INDEX) {
    os << "o" << v->orig;
  }
  os << v->co;
  return os;
}

bool Plane::operator==(const Plane &other) const
{
  return norm_exact == other.norm_exact && d_exact == other.d_exact;
}

Plane::Plane(const mpq3 &norm_exact, const mpq_class &d_exact)
    : norm_exact(norm_exact), d_exact(d_exact)
{
  norm[0] = norm_exact[0].get_d();
  norm[1] = norm_exact[1].get_d();
  norm[2] = norm_exact[2].get_d();
  d = d_exact.get_d();
}

uint32_t Plane::hash() const
{
  constexpr uint32_t h1 = 33;
  constexpr uint32_t h2 = 37;
  constexpr uint32_t h3 = 39;
  uint32_t hashx = hash_mpq_class(this->norm_exact.x);
  uint32_t hashy = hash_mpq_class(this->norm_exact.y);
  uint32_t hashz = hash_mpq_class(this->norm_exact.z);
  uint32_t hashd = hash_mpq_class(this->d_exact);
  uint32_t ans = hashx ^ (hashy * h1) ^ (hashz * h1 * h2) ^ (hashd * h1 * h2 * h3);
  return ans;
}

/* Need a canonical form of a plane so that can use as a key in a map and
 * all coplanar triangles will have the same key.
 * Make the first nonzero component of the normal be 1.
 * Note that this might flip the orientation of the plane.
 */
void Plane::make_canonical()
{
  if (norm_exact[0] != 0) {
    mpq_class den = norm_exact[0];
    norm_exact = mpq3(1, norm_exact[1] / den, norm_exact[2] / den);
    d_exact = d_exact / den;
  }
  else if (norm_exact[1] != 0) {
    mpq_class den = norm_exact[1];
    norm_exact = mpq3(0, 1, norm_exact[2] / den);
    d_exact = d_exact / den;
  }
  else {
    mpq_class den = norm_exact[2];
    norm_exact = mpq3(0, 0, 1);
    d_exact = d_exact / den;
  }
  norm = double3(norm_exact[0].get_d(), norm_exact[1].get_d(), norm_exact[2].get_d());
  d = d_exact.get_d();
}

std::ostream &operator<<(std::ostream &os, const Plane &plane)
{
  os << "[" << plane.norm << ";" << plane.d << "]";
  return os;
}

Face::Face(Span<Vertp> verts, int id, int orig, Span<int> edge_origs)
    : vert(verts), edge_orig(edge_origs), id(id), orig(orig)
{
  mpq3 normal;
  if (vert.size() > 3) {
    Array<mpq3> co(vert.size());
    for (uint i : index_range()) {
      co[i] = vert[i]->co_exact;
    }
    normal = mpq3::cross_poly(co);
  }
  else {
    mpq3 tr02 = vert[0]->co_exact - vert[2]->co_exact;
    mpq3 tr12 = vert[1]->co_exact - vert[2]->co_exact;
    normal = mpq3::cross(tr02, tr12);
  }
  mpq_class d = -mpq3::dot(normal, vert[0]->co_exact);
  plane = Plane(normal, d);
}

Face::Face(const Face &other)
    : vert(other.vert),
      edge_orig(other.edge_orig),
      plane(other.plane),
      id(other.id),
      orig(other.orig)
{
}

Face::Face(Face &&other) noexcept
    : vert(std::move(other.vert)),
      edge_orig(std::move(other.edge_orig)),
      plane(std::move(other.plane)),
      id(other.id),
      orig(other.orig)
{
}

Face &Face::operator=(const Face &other)
{
  if (this != &other) {
    this->vert = other.vert;
    this->edge_orig = other.edge_orig;
    this->plane = other.plane;
    this->id = other.id;
    this->orig = other.orig;
  }
  return *this;
}

Face &Face::operator=(Face &&other) noexcept
{
  this->vert = std::move(other.vert);
  this->edge_orig = std::move(other.edge_orig);
  this->plane = std::move(other.plane);
  this->id = other.id;
  this->orig = other.orig;
  return *this;
}

bool Face::operator==(const Face &other) const
{
  if (this->size() != other.size()) {
    return false;
  }
  for (FacePos i : index_range()) {
    /* Can test pointer equality since we will have
     * unique vert pointers for unique co_equal's.
     */
    if (this->vert[i] != other.vert[i]) {
      return false;
    }
  }
  return true;
}

bool Face::cyclic_equal(const Face &other) const
{
  if (this->size() != other.size()) {
    return false;
  }
  uint flen = this->size();
  for (FacePos start : index_range()) {
    for (FacePos start_other : index_range()) {
      bool ok = true;
      for (uint i = 0; ok && i < flen; ++i) {
        FacePos p = (start + i) % flen;
        FacePos p_other = (start_other + i) % flen;
        if (this->vert[p] != other.vert[p_other]) {
          ok = false;
        }
      }
      if (ok) {
        return true;
      }
    }
  }
  return false;
}

std::ostream &operator<<(std::ostream &os, Facep f)
{
  os << "f" << f->id << "o" << f->orig << "[";
  for (Vertp v : *f) {
    os << "v" << v->id;
    if (v->orig != NO_INDEX) {
      os << "o" << v->orig;
    }
    if (v != f->vert[f->size() - 1]) {
      os << " ";
    }
  }
  os << "]";
  if (f->orig != NO_INDEX) {
    os << "o" << f->orig;
  }
  return os;
}

/* MArena is the owner of the Vert and Face resources used
 * during a run of one of the meshintersect main functions.
 * It also keeps has a hash table of all Verts created so that it can
 * ensure that only one instance of a Vert with a given co_exact will
 * exist. I.e., it dedups the vertices.
 */
class MArena::MArenaImpl {

  /* Don't use Vert itself as key since resizing may move
   * pointers to the Vert around, and we need to have those pointers
   * stay the same throughout the lifetime of the MArena.
   */
  struct VSetKey {
    Vert *vert;

    VSetKey(Vert *p) : vert(p)
    {
    }

    uint32_t hash() const
    {
      return vert->hash();
    }

    bool operator==(const VSetKey &other) const
    {
      return *this->vert == *other.vert;
    }
  };

  VectorSet<VSetKey> vset_; /* TODO: replace with Set */

  /* Ownership of the Vert memory is here, so destroying this reclaims that memory. */
  /* TODO: replace these with pooled allocation, and just destory the pools at the end. */
  Vector<std::unique_ptr<Vert>> allocated_verts_;
  Vector<std::unique_ptr<Face>> allocated_faces_;

  /* Use these to allocate ids when Verts and Faces are allocated. */
  int next_vert_id_ = 0;
  int next_face_id_ = 0;

 public:
  MArenaImpl() = default;
  MArenaImpl(const MArenaImpl &) = delete;
  MArenaImpl(MArenaImpl &&) = delete;
  ~MArenaImpl() = default;

  void reserve(int vert_num_hint, int face_num_hint)
  {
    vset_.reserve(vert_num_hint);
    allocated_verts_.reserve(vert_num_hint);
    allocated_faces_.reserve(face_num_hint);
  }

  uint tot_allocated_verts() const
  {
    return allocated_verts_.size();
  }

  uint tot_allocated_faces() const
  {
    return allocated_faces_.size();
  }

  Vertp add_or_find_vert(const mpq3 &co, int orig)
  {
    double3 dco(co[0].get_d(), co[1].get_d(), co[2].get_d());
    return add_or_find_vert(co, dco, orig);
  }

  Vertp add_or_find_vert(const double3 &co, int orig)
  {
    mpq3 mco(co[0], co[1], co[2]);
    return add_or_find_vert(mco, co, orig);
  }

  Facep add_face(Span<Vertp> verts, int orig, Span<int> edge_origs)
  {
    Face *f = new Face(verts, next_face_id_++, orig, edge_origs);
    allocated_faces_.append(std::unique_ptr<Face>(f));
    return f;
  }

  Vertp find_vert(const mpq3 &co) const
  {
    Vert vtry(co, double3(), NO_INDEX, NO_INDEX);
    VSetKey vskey(&vtry);
    int i = vset_.index_of_try(vskey);
    if (i == -1) {
      return nullptr;
    }
    return vset_[i].vert;
  }

  /* This is slow. Only used for unit tests right now.
   * The argument vs can be a cyclic shift of the actual stored Face.
   */
  Facep find_face(Span<Vertp> vs) const
  {
    Array<int> eorig(vs.size(), NO_INDEX);
    Face ftry(vs, NO_INDEX, NO_INDEX, eorig);
    for (const uint i : allocated_faces_.index_range()) {
      if (ftry.cyclic_equal(*allocated_faces_[i])) {
        return allocated_faces_[i].get();
      }
    }
    return nullptr;
  }

 private:
  Vertp add_or_find_vert(const mpq3 &mco, const double3 &dco, int orig)
  {
    /* Don't allocate Vert yet, in case it is already there. */
    Vert vtry(mco, dco, NO_INDEX, NO_INDEX);
    VSetKey vskey(&vtry);
    int i = vset_.index_of_try(vskey);
    if (i == -1) {
      vskey.vert = new Vert(mco, dco, next_vert_id_++, orig);
      vset_.add_new(vskey);
      allocated_verts_.append(std::unique_ptr<Vert>(vskey.vert));
      return vskey.vert;
    }
    /* It was a dup, so return the existing one.
     * Note that the returned Vert may have a different orig.
     * This is the intended semantics: if the Vert already
     * exists then we are merging verts and using the first-seen
     * one as the canonical one.
     */
    return vset_[i].vert;
  };
};

MArena::MArena()
{
  pimpl_ = std::unique_ptr<MArenaImpl>(new MArenaImpl());
}

MArena::~MArena()
{
}

void MArena::reserve(int vert_num_hint, int face_num_hint)
{
  pimpl_->reserve(vert_num_hint, face_num_hint);
}

uint MArena::tot_allocated_verts() const
{
  return pimpl_->tot_allocated_verts();
}

uint MArena::tot_allocated_faces() const
{
  return pimpl_->tot_allocated_faces();
}

Vertp MArena::add_or_find_vert(const mpq3 &co, int orig)
{
  return pimpl_->add_or_find_vert(co, orig);
}

Facep MArena::add_face(Span<Vertp> verts, int orig, Span<int> edge_origs)
{
  return pimpl_->add_face(verts, orig, edge_origs);
}

Vertp MArena::add_or_find_vert(const double3 &co, int orig)
{
  return pimpl_->add_or_find_vert(co, orig);
}

Vertp MArena::find_vert(const mpq3 &co) const
{
  return pimpl_->find_vert(co);
}

Facep MArena::find_face(Span<Vertp> verts) const
{
  return pimpl_->find_face(verts);
}

void Mesh::set_faces(Span<Facep> faces)
{
  face_ = faces;
}

uint Mesh::lookup_vert(Vertp v) const
{
  BLI_assert(vert_populated_);
  return vert_to_index_.lookup_default(v, NO_INDEX_U);
}

void Mesh::populate_vert()
{
  /* This is likely an overestimate, since verts are shared between
   * faces. It is ok if estimate is over or even under.
   */
  constexpr int ESTIMATE_VERTS_PER_FACE = 4;
  uint estimate_num_verts = ESTIMATE_VERTS_PER_FACE * face_.size();
  populate_vert(estimate_num_verts);
}

void Mesh::populate_vert(uint max_verts)
{
  if (vert_populated_) {
    return;
  }
  vert_to_index_.reserve(max_verts);
  uint next_allocate_index = 0;
  for (Facep f : face_) {
    for (Vertp v : *f) {
      uint index = vert_to_index_.lookup_default(v, NO_INDEX_U);
      if (index == NO_INDEX_U) {
        BLI_assert(next_allocate_index < UINT_MAX - 2);
        vert_to_index_.add(v, next_allocate_index++);
      }
    }
  }
  uint tot_v = next_allocate_index;
  vert_ = Array<Vertp>(tot_v);
  for (auto item : vert_to_index_.items()) {
    uint index = item.value;
    BLI_assert(index < tot_v);
    vert_[index] = item.key;
  }
  /* Easier debugging (at least when there are no merged input verts)
   * if output vert order is same as input, with new verts at the end.
   * TODO: when all debugged, set fix_order = false.
   */
  const bool fix_order = true;
  if (fix_order) {
    std::sort(vert_.begin(), vert_.end(), [](Vertp a, Vertp b) {
      if (a->orig != NO_INDEX && b->orig != NO_INDEX) {
        return a->orig < b->orig;
      }
      if (a->orig != NO_INDEX) {
        return true;
      }
      if (b->orig != NO_INDEX) {
        return false;
      }
      return a->id < b->id;
    });
    for (uint i : vert_.index_range()) {
      Vertp v = vert_[i];
      vert_to_index_.add_overwrite(v, i);
    }
  }
  vert_populated_ = true;
}

void Mesh::erase_face_positions(uint f_index, Span<bool> face_pos_erase, MArena *arena)
{
  Facep cur_f = this->face(f_index);
  int cur_len = static_cast<int>(cur_f->size());
  int num_to_erase = 0;
  for (uint i : cur_f->index_range()) {
    if (face_pos_erase[i]) {
      ++num_to_erase;
    }
  }
  if (num_to_erase == 0) {
    return;
  }
  int new_len = cur_len - num_to_erase;
  if (new_len < 3) {
    /* Invalid erase. Don't do anything. */
    return;
  }
  Array<Vertp> new_vert(new_len);
  Array<int> new_edge_orig(new_len);
  int new_index = 0;
  for (uint i : cur_f->index_range()) {
    if (!face_pos_erase[i]) {
      new_vert[new_index] = (*cur_f)[i];
      new_edge_orig[new_index] = cur_f->edge_orig[i];
      ++new_index;
    }
  }
  BLI_assert(new_index == new_len);
  this->face_[f_index] = arena->add_face(new_vert, cur_f->orig, new_edge_orig);
}

std::ostream &operator<<(std::ostream &os, const Mesh &mesh)
{
  if (mesh.has_verts()) {
    os << "Verts:\n";
    int i = 0;
    for (Vertp v : mesh.vertices()) {
      os << i << ": " << v << "\n";
      ++i;
    }
  }
  os << "\nFaces:\n";
  int i = 0;
  for (Facep f : mesh.faces()) {
    os << i << ": " << f << "\n";
    os << "    plane=" << f->plane << " eorig=[";
    for (Face::FacePos p = 0; p < f->size(); ++p) {
      os << f->edge_orig[p] << " ";
    }
    os << "]\n";
    ++i;
  }
  return os;
}

struct BoundingBox {
  float3 min{FLT_MAX, FLT_MAX, FLT_MAX};
  float3 max{-FLT_MAX, -FLT_MAX, -FLT_MAX};

  BoundingBox() = default;
  BoundingBox(const float3 &min, const float3 &max) : min(min), max(max)
  {
  }
  BoundingBox(const BoundingBox &other) : min(other.min), max(other.max)
  {
  }
  BoundingBox(BoundingBox &&other) noexcept : min(std::move(other.min)), max(std::move(other.max))
  {
  }
  ~BoundingBox() = default;
  BoundingBox operator=(const BoundingBox &other)
  {
    if (this != &other) {
      min = other.min;
      max = other.max;
    }
    return *this;
  }
  BoundingBox operator=(BoundingBox &&other) noexcept
  {
    min = std::move(other.min);
    max = std::move(other.max);
    return *this;
  }

  void combine(const float3 &p)
  {
    min.x = min_ff(min.x, p.x);
    min.y = min_ff(min.y, p.y);
    min.z = min_ff(min.z, p.z);
    max.x = max_ff(max.x, p.x);
    max.y = max_ff(max.y, p.y);
    max.z = max_ff(max.z, p.z);
  }

  void combine(const double3 &p)
  {
    min.x = min_ff(min.x, static_cast<float>(p.x));
    min.y = min_ff(min.y, static_cast<float>(p.y));
    min.z = min_ff(min.z, static_cast<float>(p.z));
    max.x = max_ff(max.x, static_cast<float>(p.x));
    max.y = max_ff(max.y, static_cast<float>(p.y));
    max.z = max_ff(max.z, static_cast<float>(p.z));
  }

  void combine(const BoundingBox &bb)
  {
    min.x = min_ff(min.x, bb.min.x);
    min.y = min_ff(min.y, bb.min.y);
    min.z = min_ff(min.z, bb.min.z);
    max.x = max_ff(max.x, bb.max.x);
    max.y = max_ff(max.y, bb.max.y);
    max.z = max_ff(max.z, bb.max.z);
  }

  void expand(float pad)
  {
    min.x -= pad;
    min.y -= pad;
    min.z -= pad;
    max.x += pad;
    max.y += pad;
    max.z += pad;
  }
};

/* Assume bounding boxes have been expanded by a sufficient epislon on all sides
 * so that the comparisons against the bb bounds are sufficient to guarantee that
 * if an overlap or even touching could happen, this will return true.
 */
static bool bbs_might_intersect(const BoundingBox &bb_a, const BoundingBox &bb_b)
{
  return isect_aabb_aabb_v3(bb_a.min, bb_a.max, bb_b.min, bb_b.max);
}

/* We will expand the bounding boxes by an epsilon on all sides so that
 * the "less than" tests in isect_aabb_aabb_v3 are sufficient to detect
 * touching or overlap.
 */
static Array<BoundingBox> calc_face_bounding_boxes(const Mesh &m)
{
  double max_abs_val = 0.0;
  Array<BoundingBox> ans(m.face_size());
  for (uint f : m.face_index_range()) {
    const Face &face = *m.face(f);
    BoundingBox &bb = ans[f];
    for (Vertp v : face) {
      bb.combine(v->co);
      for (int i = 0; i < 3; ++i) {
        max_abs_val = max_dd(max_abs_val, fabs(v->co[i]));
      }
    }
  }
  float pad;
  constexpr float pad_factor = 10.0f;
  if (max_abs_val == 0.0f) {
    pad = FLT_EPSILON;
  }
  else {
    pad = 2 * FLT_EPSILON * max_abs_val;
  }
  pad *= pad_factor; /* For extra safety. */
  for (uint f : m.face_index_range()) {
    ans[f].expand(pad);
  }
  return ans;
}

/* A cluster of coplanar triangles, by index.
 * A pair of triangles T0 and T1 is said to "nontrivially coplanar-intersect"
 * if they are coplanar, intersect, and their intersection is not just existing
 * elements (verts, edges) of both triangles.
 * A coplanar cluster is said to be "nontrivial" if it has more than one triangle
 * and every triangle in it nontrivially coplanar-intersects with at least one other
 * triangle in the cluster.
 */
class CoplanarCluster {
  Vector<uint> tris_;
  BoundingBox bb_;

 public:
  CoplanarCluster() = default;
  CoplanarCluster(uint t, const BoundingBox &bb)
  {
    this->add_tri(t, bb);
  }
  CoplanarCluster(const CoplanarCluster &other) : tris_(other.tris_), bb_(other.bb_)
  {
  }
  CoplanarCluster(CoplanarCluster &&other) noexcept
      : tris_(std::move(other.tris_)), bb_(std::move(other.bb_))
  {
  }
  ~CoplanarCluster() = default;
  CoplanarCluster &operator=(const CoplanarCluster &other)
  {
    if (this != &other) {
      tris_ = other.tris_;
      bb_ = other.bb_;
    }
    return *this;
  }
  CoplanarCluster &operator=(CoplanarCluster &&other) noexcept
  {
    tris_ = std::move(other.tris_);
    bb_ = std::move(other.bb_);
    return *this;
  }

  /* Assume that caller knows this will not be a duplicate. */
  void add_tri(uint t, const BoundingBox &bb)
  {
    tris_.append(t);
    bb_ = bb;
  }
  uint tot_tri() const
  {
    return tris_.size();
  }
  uint tri(uint index) const
  {
    return tris_[index];
  }
  const uint *begin() const
  {
    return tris_.begin();
  }
  const uint *end() const
  {
    return tris_.end();
  }

  const BoundingBox &bounding_box() const
  {
    return bb_;
  }
};

/* Maintains indexed set of CoplanarCluster, with the added ability
 * to efficiently find the cluster index of any given triangle
 * (the max triangle index needs to be given in the initializer).
 * The tri_cluster(t) function returns -1 if t is not part of any cluster.
 */
class CoplanarClusterInfo {
  Vector<CoplanarCluster> clusters_;
  Array<uint> tri_cluster_;

 public:
  CoplanarClusterInfo() = default;
  explicit CoplanarClusterInfo(uint numtri) : tri_cluster_(Array<uint>(numtri))
  {
    tri_cluster_.fill(-1);
  }

  uint tri_cluster(uint t) const
  {
    BLI_assert(t < tri_cluster_.size());
    return tri_cluster_[t];
  }

  uint add_cluster(CoplanarCluster cl)
  {
    uint c_index = clusters_.append_and_get_index(cl);
    for (uint t : cl) {
      BLI_assert(t < tri_cluster_.size());
      tri_cluster_[t] = c_index;
    }
    return c_index;
  }

  uint tot_cluster() const
  {
    return clusters_.size();
  }

  const CoplanarCluster *begin()
  {
    return clusters_.begin();
  }

  const CoplanarCluster *end()
  {
    return clusters_.end();
  }

  IndexRange index_range() const
  {
    return clusters_.index_range();
  }

  const CoplanarCluster &cluster(uint index) const
  {
    BLI_assert(index < clusters_.size());
    return clusters_[index];
  }
};

static std::ostream &operator<<(std::ostream &os, const CoplanarCluster &cl);

static std::ostream &operator<<(std::ostream &os, const CoplanarClusterInfo &clinfo);

enum ITT_value_kind { INONE, IPOINT, ISEGMENT, ICOPLANAR };

struct ITT_value {
  enum ITT_value_kind kind;
  mpq3 p1;      /* Only relevant for IPOINT and ISEGMENT kind. */
  mpq3 p2;      /* Only relevant for ISEGMENT kind. */
  int t_source; /* Index of the source triangle that intersected the target one. */

  ITT_value() : kind(INONE), t_source(-1)
  {
  }
  ITT_value(ITT_value_kind k) : kind(k), t_source(-1)
  {
  }
  ITT_value(ITT_value_kind k, int tsrc) : kind(k), t_source(tsrc)
  {
  }
  ITT_value(ITT_value_kind k, const mpq3 &p1) : kind(k), p1(p1), t_source(-1)
  {
  }
  ITT_value(ITT_value_kind k, const mpq3 &p1, const mpq3 &p2)
      : kind(k), p1(p1), p2(p2), t_source(-1)
  {
  }
  ITT_value(const ITT_value &other)
      : kind(other.kind), p1(other.p1), p2(other.p2), t_source(other.t_source)
  {
  }
  ITT_value(ITT_value &&other) noexcept
      : kind(other.kind),
        p1(std::move(other.p1)),
        p2(std::move(other.p2)),
        t_source(other.t_source)
  {
  }
  ~ITT_value()
  {
  }
  ITT_value &operator=(const ITT_value &other)
  {
    if (this != &other) {
      kind = other.kind;
      p1 = other.p1;
      p2 = other.p2;
      t_source = other.t_source;
    }
    return *this;
  }
  ITT_value &operator=(ITT_value &&other) noexcept
  {
    kind = other.kind;
    p1 = std::move(other.p1);
    p2 = std::move(other.p2);
    t_source = other.t_source;
    return *this;
  }
};

static std::ostream &operator<<(std::ostream &os, const ITT_value &itt);

/* Project a 3d vert to a 2d one by eliding proj_axis. This does not create
 * degeneracies as long as the projection axis is one where the corresponding
 * component of the originating plane normal is non-zero.
 */
static mpq2 project_3d_to_2d(const mpq3 &p3d, int proj_axis)
{
  mpq2 p2d;
  switch (proj_axis) {
    case (0): {
      p2d[0] = p3d[1];
      p2d[1] = p3d[2];
    } break;
    case (1): {
      p2d[0] = p3d[0];
      p2d[1] = p3d[2];
    } break;
    case (2): {
      p2d[0] = p3d[0];
      p2d[1] = p3d[1];
    } break;
    default:
      BLI_assert(false);
  }
  return p2d;
}

/* Is a point in the interior of a 2d triangle or on one of its
 * edges but not either endpoint of the edge?
 * orient[pi][i] is the orientation test of the point pi against
 * the side of the triangle starting at index i.
 * Assume the triangele is non-degenerate and CCW-oriented.
 * Then answer is true if p is left of or on all three of triangle a's edges,
 * and strictly left of at least on of them.
 */
static bool non_trivially_2d_point_in_tri(const int orients[3][3], int pi)
{
  int p_left_01 = orients[pi][0];
  int p_left_12 = orients[pi][1];
  int p_left_20 = orients[pi][2];
  return (p_left_01 >= 0 && p_left_12 >= 0 && p_left_20 >= 0 &&
          (p_left_01 + p_left_12 + p_left_20) >= 2);
}

/* Given orients as defined in non_trivially_2d_intersect, do the triangles
 * overlap in a "hex" pattern? That is, the overlap region is a hexagon, which
 * one gets by having, each point of one triangle being strictly rightof one
 * edge of the other and strictly left of the other two edges; and vice versa.
 */
static bool non_trivially_2d_hex_overlap(int orients[2][3][3])
{
  for (int ab = 0; ab < 2; ++ab) {
    for (int i = 0; i < 3; ++i) {
      bool ok = orients[ab][i][0] + orients[ab][i][1] + orients[ab][i][2] == 1 &&
                orients[ab][i][0] != 0 && orients[ab][i][1] != 0 && orients[i][2] != 0;
      if (!ok) {
        return false;
      }
    }
  }
  return true;
}

/* Given orients as defined in non_trivially_2d_intersect, do the triangles
 * have one shared edge in a "folded-over" configuration?
 * As well as a shared edge, the third vertex of one triangle needs to be
 * rightof one and leftof the other two edges of the other triangle.
 */
static bool non_trivially_2d_shared_edge_overlap(int orients[2][3][3],
                                                 const mpq2 *a[3],
                                                 const mpq2 *b[3])
{
  for (int i = 0; i < 3; ++i) {
    int in = (i + 1) % 3;
    int inn = (i + 2) % 3;
    for (int j = 0; j < 3; ++j) {
      int jn = (j + 1) % 3;
      int jnn = (j + 2) % 3;
      if (*a[i] == *b[j] && *a[in] == *b[jn]) {
        /* Edge from a[i] is shared with edge from b[j]. */
        /* See if a[inn] is rightof or on one of the other edges of b.
         * If it is on, then it has to be rightof or leftof the shared edge,
         * depending on which edge it is.
         */
        if (orients[0][inn][jn] < 0 || orients[0][inn][jnn] < 0) {
          return true;
        }
        if (orients[0][inn][jn] == 0 && orients[0][inn][j] == 1) {
          return true;
        }
        if (orients[0][inn][jnn] == 0 && orients[0][inn][j] == -1) {
          return true;
        }
        /* Similarly for b[jnn]. */
        if (orients[1][jnn][in] < 0 || orients[1][jnn][inn] < 0) {
          return true;
        }
        if (orients[1][jnn][in] == 0 && orients[1][jnn][i] == 1) {
          return true;
        }
        if (orients[1][jnn][inn] == 0 && orients[1][jnn][i] == -1) {
          return true;
        }
      }
    }
  }
  return false;
}

/* Are the triangles the same, perhaps with some permutation of vertices? */
static bool same_triangles(const mpq2 *a[3], const mpq2 *b[3])
{
  for (int i = 0; i < 3; ++i) {
    if (a[0] == b[i] && a[1] == b[(i + 1) % 3] && a[2] == b[(i + 2) % 3]) {
      return true;
    }
  }
  return false;
}

/* Do 2d triangles (a[0], a[1], a[2]) and (b[0], b[1], b2[2]) intersect at more than just shared
 * vertices or a shared edge? This is true if any point of one tri is non-trivially inside the
 * other. NO: that isn't quite sufficient: there is also the case where the verts are all mutually
 * outside the other's triangle, but there is a hexagonal overlap region where they overlap.
 */
static bool non_trivially_2d_intersect(const mpq2 *a[3], const mpq2 *b[3])
{
  /* TODO: Could experiment with trying bounding box tests before these.
   * TODO: Find a less expensive way than 18 orient tests to do this.
   */
  /* orients[0][ai][bi] is orient of point a[ai] compared to seg starting at b[bi].
   * orients[1][bi][ai] is orient of point b[bi] compared to seg starting at a[ai].
   */
  int orients[2][3][3];
  for (int ab = 0; ab < 2; ++ab) {
    for (int ai = 0; ai < 3; ++ai) {
      for (int bi = 0; bi < 3; ++bi) {
        if (ab == 0) {
          orients[0][ai][bi] = mpq2::orient2d(*b[bi], *b[(bi + 1) % 3], *a[ai]);
        }
        else {
          orients[1][bi][ai] = mpq2::orient2d(*a[ai], *a[(ai + 1) % 3], *b[bi]);
        }
      }
    }
  }
  return non_trivially_2d_point_in_tri(orients[0], 0) ||
         non_trivially_2d_point_in_tri(orients[0], 1) ||
         non_trivially_2d_point_in_tri(orients[0], 2) ||
         non_trivially_2d_point_in_tri(orients[1], 0) ||
         non_trivially_2d_point_in_tri(orients[1], 1) ||
         non_trivially_2d_point_in_tri(orients[1], 2) || non_trivially_2d_hex_overlap(orients) ||
         non_trivially_2d_shared_edge_overlap(orients, a, b) || same_triangles(a, b);
  return true;
}

/* Does triangle t in tm non-trivially non-coplanar intersect any triangle
 * in CoplanarCluster cl? Assume t is known to be in the same plane as all
 * the triangles in cl, and that proj_axis is a good axis to project down
 * to solve this problem in 2d.
 */
static bool non_trivially_coplanar_intersects(const Mesh &tm,
                                              uint t,
                                              const CoplanarCluster &cl,
                                              int proj_axis)
{
  const Face &tri = *tm.face(t);
  mpq2 v0 = project_3d_to_2d(tri[0]->co_exact, proj_axis);
  mpq2 v1 = project_3d_to_2d(tri[1]->co_exact, proj_axis);
  mpq2 v2 = project_3d_to_2d(tri[2]->co_exact, proj_axis);
  if (mpq2::orient2d(v0, v1, v2) != 1) {
    mpq2 tmp = v1;
    v1 = v2;
    v2 = tmp;
  }
  for (const uint cl_t : cl) {
    const Face &cl_tri = *tm.face(cl_t);
    mpq2 ctv0 = project_3d_to_2d(cl_tri[0]->co_exact, proj_axis);
    mpq2 ctv1 = project_3d_to_2d(cl_tri[1]->co_exact, proj_axis);
    mpq2 ctv2 = project_3d_to_2d(cl_tri[2]->co_exact, proj_axis);
    if (mpq2::orient2d(ctv0, ctv1, ctv2) != 1) {
      mpq2 tmp = ctv1;
      ctv1 = ctv2;
      ctv2 = tmp;
    }
    const mpq2 *v[] = {&v0, &v1, &v2};
    const mpq2 *ctv[] = {&ctv0, &ctv1, &ctv2};
    if (non_trivially_2d_intersect(v, ctv)) {
      return true;
    }
  }
  return false;
}

/* Keeping this code for a while, but for now, almost all
 * trivial intersects are found before calling intersect_tri_tri now.
 */
#if 0
/* Do tri1 and tri2 intersect at all, and if so, is the intersection
 * something other than a common vertex or a common edge?
 * The itt value is the result of calling intersect_tri_tri on tri1, tri2.
 */
static bool non_trivial_intersect(const ITT_value &itt, Facep tri1, Facep tri2)
{
  if (itt.kind == INONE) {
    return false;
  }
  Facep tris[2] = {tri1, tri2};
  if (itt.kind == IPOINT) {
    bool has_p_as_vert[2] {false, false};
    for (int i = 0; i < 2; ++i) {
      for (Vertp v : *tris[i]) {
        if (itt.p1 == v->co_exact) {
          has_p_as_vert[i] = true;
          break;
        }
      }
    }
    return !(has_p_as_vert[0] && has_p_as_vert[1]);
  }
  if (itt.kind == ISEGMENT) {
    bool has_seg_as_edge[2] = {false, false};
    for (int i = 0; i < 2; ++i) {
      const Face &t = *tris[i];
      for (uint pos : t.index_range()) {
        uint nextpos = t.next_pos(pos);
        if ((itt.p1 == t[pos]->co_exact && itt.p2 == t[nextpos]->co_exact) ||
            (itt.p2 == t[pos]->co_exact && itt.p1 == t[nextpos]->co_exact)) {
          has_seg_as_edge[i] = true;
          break;
        }
      }
    }
    return !(has_seg_as_edge[0] && has_seg_as_edge[1]);
  }
  BLI_assert(itt.kind == ICOPLANAR);
  /* TODO: refactor this common code with code above. */
  int proj_axis = mpq3::dominant_axis(tri1->plane.norm_exact);
  mpq2 tri_2d[2][3];
  for (int i = 0; i < 2; ++i) {
    mpq2 v0 = project_3d_to_2d((*tris[i])[0]->co_exact, proj_axis);
    mpq2 v1 = project_3d_to_2d((*tris[i])[1]->co_exact, proj_axis);
    mpq2 v2 = project_3d_to_2d((*tris[i])[2]->co_exact, proj_axis);
    if (mpq2::orient2d(v0, v1, v2) != 1) {
      mpq2 tmp = v1;
      v1 = v2;
      v2 = tmp;
    }
    tri_2d[i][0] = v0;
    tri_2d[i][1] = v1;
    tri_2d[i][2] = v2;
  }
  const mpq2 *va[] = {&tri_2d[0][0], &tri_2d[0][1], &tri_2d[0][2]};
  const mpq2 *vb[] = {&tri_2d[1][0], &tri_2d[1][1], &tri_2d[1][2]};
  return non_trivially_2d_intersect(va, vb);
}
#endif

/* The sup and index functions are defined in the paper:
 * EXACT GEOMETRIC COMPUTATION USING CASCADING, by
 * Burnikel, Funke, and Seel. They are used to find absolute
 * bounds on the error due to doing a calculation in double
 * instead of exactly. For calculations involving only +, -, and *,
 * the supremum is the same function except using absolute values
 * on inputs and using + instead of -.
 * The index function follows these rules:
 *    index(x op y) = 1 + max(index(x), index(y)) for op + or -
 *    index(x * y)  = 1 + index(x) + index(y)
 *    index(x) = 0 if input x can be respresented exactly as a double
 *    index(x) = 1 otherwise.
 *
 * With these rules in place, we know an absolute error bound:
 *
 *     |E_exact - E| <= supremum(E) * index(E) * DBL_EPSILON
 *
 * where E_exact is what would have been the exact value of the
 * expression and E is the one calculated with doubles.
 *
 * So the sign of E is the same as the sign of E_exact if
 *    |E| > supremum(E) * index(E) * DBL_EPSILON
 *
 * Note: a possible speedup would be to have a simple function
 * that calculates the error bound if one knows that all values
 * are less than some global maximum - most of the function would
 * be calculated ahead of time. The global max could be passed
 * from above.
 */

static double supremum_cross(const double3 &a, const double3 &b)
{
  double3 abs_a{fabs(a[0]), fabs(a[1]), fabs(a[2])};
  double3 abs_b{fabs(b[0]), fabs(b[1]), fabs(b[2])};
  double3 c;
  /* This is cross(a, b) but using absoluate values for a and b
   * and always using + when operation is + or -.
   */
  c[0] = a[1] * b[2] + a[2] * b[1];
  c[1] = a[2] * b[0] + a[0] * b[2];
  c[2] = a[0] * b[1] + a[1] * b[0];
  return double3::dot(c, c);
}

/* Used with supremum to get error bound. See Burnikel et al paper.
 * In cases where argument coords are known to be exactly
 * representable in doubles, this value is 7 instead of 11.
 */
constexpr int index_cross = 11;

static double supremum_dot(const double3 &a, const double3 &b)
{
  double3 abs_a{fabs(a[0]), fabs(a[1]), fabs(a[2])};
  double3 abs_b{fabs(b[0]), fabs(b[1]), fabs(b[2])};
  return double3::dot(abs_a, abs_b);
}

/* This value would be 3 if input values are exact */
static int index_dot = 5;

static double supremum_orient3d(const double3 &a,
                                const double3 &b,
                                const double3 &c,
                                const double3 &d)
{
  double3 abs_a{fabs(a[0]), fabs(a[1]), fabs(a[2])};
  double3 abs_b{fabs(b[0]), fabs(b[1]), fabs(b[2])};
  double3 abs_c{fabs(c[0]), fabs(c[1]), fabs(c[2])};
  double3 abs_d{fabs(d[0]), fabs(d[1]), fabs(d[2])};
  double adx = abs_a[0] + abs_d[0];
  double bdx = abs_b[0] + abs_d[0];
  double cdx = abs_c[0] + abs_d[0];
  double ady = abs_a[1] + abs_d[1];
  double bdy = abs_b[1] + abs_d[1];
  double cdy = abs_c[1] + abs_d[1];
  double adz = abs_a[2] + abs_d[2];
  double bdz = abs_b[2] + abs_d[2];
  double cdz = abs_c[2] + abs_d[2];

  double bdxcdy = bdx * cdy;
  double cdxbdy = cdx * bdy;

  double cdxady = cdx * ady;
  double adxcdy = adx * cdy;

  double adxbdy = adx * bdy;
  double bdxady = bdx * ady;

  double det = adz * (bdxcdy + cdxbdy) + bdz * (cdxady + adxcdy) + cdz * (adxbdy + bdxady);
  return det;
}

/* This value would be 8 if the input values are exact. */
static int index_orient3d = 11;

/* Return the approximate orient3d of the four double3's, with
 * the guarantee that if the value is -1 or 1 then the underlying
 * mpq3 test would also have returned that value.
 * When the return value is 0, we are not sure of the sign.
 */
int fliter_orient3d(const double3 &a, const double3 &b, const double3 &c, const double3 &d)
{
  double o3dfast = double3::orient3d_fast(a, b, c, d);
  if (o3dfast == 0.0) {
    return 0;
  }
  double err_bound = supremum_orient3d(a, b, c, d) * index_orient3d * DBL_EPSILON;
  if (fabs(o3dfast) > err_bound) {
    return o3dfast > 0.0 ? 1 : -1;
  }
  return 0;
}

/* Return the approximate orient3d of the tri plane points and v, with
 * the guarantee that if the value is -1 or 1 then the underlying
 * mpq3 test would also have returned that value.
 * When the return value is 0, we are not sure of the sign.
 */
int filter_tri_plane_vert_orient3d(const Face &tri, Vertp v)
{
  return fliter_orient3d(tri[0]->co, tri[1]->co, tri[2]->co, v->co);
}

/* Are vectors a and b parallel or nearly parallel?
 * This routine should only return false if we are certain
 * that they are not parallel, taking into account the
 * possible numeric errors and input value approximation.
 */
static bool near_parallel_vecs(const double3 &a, const double3 &b)
{
  double3 cr = double3::cross_high_precision(a, b);
  double cr_len_sq = cr.length_squared();
  if (cr_len_sq == 0.0) {
    return true;
  }
  double err_bound = supremum_cross(a, b) * index_cross * DBL_EPSILON;
  if (cr_len_sq > err_bound) {
    return false;
  }
  return true;
}

/* Return true if we are sure that dot(a,b) > 0, taking into
 * account the error bounds due to numeric errors and input value
 * approximation.
 */
static bool dot_must_be_positive(const double3 &a, const double3 &b)
{
  double d = double3::dot(a, b);
  if (d <= 0.0) {
    return false;
  }
  double err_bound = supremum_dot(a, b) * index_dot * DBL_EPSILON;
  if (d > err_bound) {
    return true;
  }
  return false;
}

/* A fast, non-exhaustive test for non_trivial intersection.
 * If this returns false then we are sure that tri1 and tri2
 * do not intersect. If it returns true, they may or may not
 * non-trivially intersect.
 * We assume that boundinb box overlap tests have already been
 * done, so don't repeat those here. This routine is checking
 * for the very common cases (when doing mesh self-intersect)
 * where triangles share an edge or a vertex, but don't
 * otherwise intersect.
 */
static bool may_non_trivially_intersect(Facep t1, Facep t2)
{
  const Face &tri1 = *t1;
  const Face &tri2 = *t2;
  Face::FacePos share1_pos[3];
  Face::FacePos share2_pos[3];
  int n_shared = 0;
  for (Face::FacePos p1 = 0; p1 < 3; ++p1) {
    Vertp v1 = tri1[p1];
    for (Face::FacePos p2 = 0; p2 < 3; ++p2) {
      Vertp v2 = tri2[p2];
      if (v1 == v2) {
        share1_pos[n_shared] = p1;
        share2_pos[n_shared] = p2;
        ++n_shared;
      }
    }
  }
  if (n_shared == 2) {
    /* t1 and t2 share an entire edge.
     * If their normals are not parallel, they cannot non-trivially intersect.
     */
    if (!near_parallel_vecs(tri1.plane.norm, tri2.plane.norm)) {
      return false;
    }
    /* The normals are parallel or nearly parallel.
     * If the normals are in the same direction and the edges have opposite
     * directions in the two triangles, they cannot non-trivially intersect.
     */
    bool erev1 = tri1.prev_pos(share1_pos[0]) == share1_pos[1];
    bool erev2 = tri2.prev_pos(share2_pos[0]) == share2_pos[1];
    if (erev1 != erev2 && dot_must_be_positive(tri1.plane.norm, tri2.plane.norm)) {
      return false;
    }
  }
  else if (n_shared == 1) {
    /* t1 and t2 share a vertex, but not an entire edge.
     * If the two non-shared verts of t2 are both on the same
     * side of tri1's plane, then they cannot non-trivially intersect.
     * (There are some other cases that could be caught here but
     * they are more expensive to check).
     */
    Face::FacePos p = share2_pos[0];
    Vertp v2a = p == 0 ? tri2[1] : tri2[0];
    Vertp v2b = (p == 0 || p == 1) ? tri2[2] : tri2[1];
    int o1 = filter_tri_plane_vert_orient3d(tri1, v2a);
    int o2 = filter_tri_plane_vert_orient3d(tri1, v2b);
    if (o1 == o2 && o1 != 0) {
      return false;
    }
    p = share1_pos[0];
    Vertp v1a = (p == 0 || p == 1) ? tri1[2] : tri1[1];
    Vertp v1b = (p == 0 || p == 1) ? tri1[2] : tri1[1];
    o1 = filter_tri_plane_vert_orient3d(tri2, v1a);
    o2 = filter_tri_plane_vert_orient3d(tri2, v1b);
    if (o1 == o2 && o1 != 0) {
      return false;
    }
  }
  /* We weren't able to prove that any intersection is trivial. */
  return true;
}

/*
 * interesect_tri_tri and helper functions.
 * This code uses the algorithm of Guigue and Devillers, as described
 * in "Faster Triangle-Triangle Intersection Tests".
 * Adapted from github code by Eric Haines:
 * github.com/erich666/jgt-code/tree/master/Volume_08/Number_1/Guigue2003
 */

/* Helper function for intersect_tri_tri. Args have been fully canonicalized
 * and we can construct the segment of intersection (triangles not coplanar).
 */

static ITT_value itt_canon2(const mpq3 &p1,
                            const mpq3 &q1,
                            const mpq3 &r1,
                            const mpq3 &p2,
                            const mpq3 &q2,
                            const mpq3 &r2,
                            const mpq3 &n1,
                            const mpq3 &n2)
{
  constexpr int dbg_level = 0;
  mpq3 source;
  mpq3 target;
  mpq_class alpha;
  bool ans_ok = false;

  mpq3 v1 = q1 - p1;
  mpq3 v2 = r2 - p1;
  mpq3 n = mpq3::cross(v1, v2);
  mpq3 v = p2 - p1;
  if (dbg_level > 1) {
    std::cout << "itt_canon2:\n";
    std::cout << "p1=" << p1 << " q1=" << q1 << " r1=" << r1 << "\n";
    std::cout << "p2=" << p2 << " q2=" << q2 << " r2=" << r2 << "\n";
    std::cout << "v=" << v << " n=" << n << "\n";
  }
  if (mpq3::dot(v, n) > 0) {
    v1 = r1 - p1;
    n = mpq3::cross(v1, v2);
    if (dbg_level > 1) {
      std::cout << "case 1: v1=" << v1 << " v2=" << v2 << " n=" << n << "\n";
    }
    if (mpq3::dot(v, n) <= 0) {
      v2 = q2 - p1;
      n = mpq3::cross(v1, v2);
      if (dbg_level > 1) {
        std::cout << "case 1a: v2=" << v2 << " n=" << n << "\n";
      }
      if (mpq3::dot(v, n) > 0) {
        v1 = p1 - p2;
        v2 = p1 - r1;
        alpha = mpq3::dot(v1, n2) / mpq3::dot(v2, n2);
        v1 = v2 * alpha;
        source = p1 - v1;
        v1 = p2 - p1;
        v2 = p2 - r2;
        alpha = mpq3::dot(v1, n1) / mpq3::dot(v2, n1);
        v1 = v2 * alpha;
        target = p2 - v1;
        ans_ok = true;
      }
      else {
        v1 = p2 - p1;
        v2 = p2 - q2;
        alpha = mpq3::dot(v1, n1) / mpq3::dot(v2, n1);
        v1 = v2 * alpha;
        source = p2 - v1;
        v1 = p2 - p1;
        v2 = p2 - r2;
        alpha = mpq3::dot(v1, n1) / mpq3::dot(v2, n1);
        v1 = v2 * alpha;
        target = p2 - v1;
        ans_ok = true;
      }
    }
    else {
      if (dbg_level > 1) {
        std::cout << "case 1b: ans=false\n";
      }
      ans_ok = false;
    }
  }
  else {
    v2 = q2 - p1;
    n = mpq3::cross(v1, v2);
    if (dbg_level > 1) {
      std::cout << "case 2: v1=" << v1 << " v2=" << v2 << " n=" << n << "\n";
    }
    if (mpq3::dot(v, n) < 0) {
      if (dbg_level > 1) {
        std::cout << "case 2a: ans=false\n";
      }
      ans_ok = false;
    }
    else {
      v1 = r1 - p1;
      n = mpq3::cross(v1, v2);
      if (dbg_level > 1) {
        std::cout << "case 2b: v1=" << v1 << " v2=" << v2 << " n=" << n << "\n";
      }
      if (mpq3::dot(v, n) > 0) {
        v1 = p1 - p2;
        v2 = p1 - r1;
        alpha = mpq3::dot(v1, n2) / mpq3::dot(v2, n2);
        v1 = v2 * alpha;
        source = p1 - v1;
        v1 = p1 - p2;
        v2 = p1 - q1;
        alpha = mpq3::dot(v1, n2) / mpq3::dot(v2, n2);
        v1 = v2 * alpha;
        target = p1 - v1;
        ans_ok = true;
      }
      else {
        v1 = p2 - p1;
        v2 = p2 - q2;
        alpha = mpq3::dot(v1, n1) / mpq3::dot(v2, n1);
        v1 = v2 * alpha;
        source = p2 - v1;
        v1 = p1 - p2;
        v2 = p1 - q1;
        alpha = mpq3::dot(v1, n2) / mpq3::dot(v2, n2);
        v1 = v2 * alpha;
        target = p1 - v1;
        ans_ok = true;
      }
    }
  }

  if (dbg_level > 0) {
    if (ans_ok) {
      std::cout << "intersect: " << source << ", " << target << "\n";
    }
    else {
      std::cout << "no intersect\n";
    }
  }
  if (ans_ok) {
    if (source == target) {
      return ITT_value(IPOINT, source);
    }
    return ITT_value(ISEGMENT, source, target);
  }
  return ITT_value(INONE);
}

/* Helper function for intersect_tri_tri. Args have been canonicalized for triangle 1. */

static ITT_value itt_canon1(const mpq3 &p1,
                            const mpq3 &q1,
                            const mpq3 &r1,
                            const mpq3 &p2,
                            const mpq3 &q2,
                            const mpq3 &r2,
                            const mpq3 &n1,
                            const mpq3 &n2,
                            int sp2,
                            int sq2,
                            int sr2)
{
  constexpr int dbg_level = 0;
  if (sp2 > 0) {
    if (sq2 > 0) {
      return itt_canon2(p1, r1, q1, r2, p2, q2, n1, n2);
    }
    if (sr2 > 0) {
      return itt_canon2(p1, r1, q1, q2, r2, p2, n1, n2);
    }
    return itt_canon2(p1, q1, r1, p2, q2, r2, n1, n2);
  }
  if (sp2 < 0) {
    if (sq2 < 0) {
      return itt_canon2(p1, q1, r1, r2, p2, q2, n1, n2);
    }
    if (sr2 < 0) {
      return itt_canon2(p1, q1, r1, q2, r2, p2, n1, n2);
    }
    return itt_canon2(p1, r1, q1, p2, q2, r2, n1, n2);
  }
  if (sq2 < 0) {
    if (sr2 >= 0) {
      return itt_canon2(p1, r1, q1, q2, r2, p2, n1, n2);
    }
    return itt_canon2(p1, q1, r1, p2, q2, r2, n1, n2);
  }
  if (sq2 > 0) {
    if (sr2 > 0) {
      return itt_canon2(p1, r1, q1, p2, q2, r2, n1, n2);
    }
    return itt_canon2(p1, q1, r1, q2, r2, p2, n1, n2);
  }
  if (sr2 > 0) {
    return itt_canon2(p1, q1, r1, r2, p2, q2, n1, n2);
  }
  if (sr2 < 0) {
    return itt_canon2(p1, r1, q1, r2, p2, q2, n1, n2);
  }
  if (dbg_level > 0) {
    std::cout << "triangles are coplanar\n";
  }
  return ITT_value(ICOPLANAR);
}

static ITT_value intersect_tri_tri(const Mesh &tm, uint t1, uint t2)
{
  constexpr int dbg_level = 0;
#ifdef PERFDEBUG
  incperfcount(0);
#endif
  const Face &tri1 = *tm.face(t1);
  const Face &tri2 = *tm.face(t2);
  Vertp vp1 = tri1[0];
  Vertp vq1 = tri1[1];
  Vertp vr1 = tri1[2];
  Vertp vp2 = tri2[0];
  Vertp vq2 = tri2[1];
  Vertp vr2 = tri2[2];
  if (dbg_level > 0) {
    std::cout << "\nINTERSECT_TRI_TRI t1=" << t1 << ", t2=" << t2 << "\n";
    std::cout << "  p1 = " << vp1 << "\n";
    std::cout << "  q1 = " << vq1 << "\n";
    std::cout << "  r1 = " << vr1 << "\n";
    std::cout << "  p2 = " << vp2 << "\n";
    std::cout << "  q2 = " << vq2 << "\n";
    std::cout << "  r2 = " << vr2 << "\n";
  }

  /* TODO: try doing intersect with double arithmetic first, with error bounds. */
  const mpq3 &p1 = vp1->co_exact;
  const mpq3 &q1 = vq1->co_exact;
  const mpq3 &r1 = vr1->co_exact;
  const mpq3 &p2 = vp2->co_exact;
  const mpq3 &q2 = vq2->co_exact;
  const mpq3 &r2 = vr2->co_exact;

  /* Get signs of t1's vertices' distances to plane of t2. */
  /* If don't have normal, use mpq3 n2 = cross_v3v3(sub_v3v3(p2, r2), sub_v3v3(q2, r2)); */
  const mpq3 &n2 = tri2.plane.norm_exact;
  int sp1 = sgn(mpq3::dot(p1 - r2, n2));
  int sq1 = sgn(mpq3::dot(q1 - r2, n2));
  int sr1 = sgn(mpq3::dot(r1 - r2, n2));

  if (dbg_level > 1) {
    std::cout << "  sp1=" << sp1 << " sq1=" << sq1 << " sr1=" << sr1 << "\n";
  }

  if ((sp1 * sq1 > 0) && (sp1 * sr1 > 0)) {
    if (dbg_level > 0) {
      std::cout << "no intersection, all t1's verts above or below t2\n";
    }
#ifdef PERFDEBUG
    incperfcount(2);
#endif
    return ITT_value(INONE);
  }

  /* Repeat for signs of t2's vertices with respect to plane of t1. */
  /* If don't have normal, use mpq3 n1 = cross_v3v3(sub_v3v3(q1, p1), sub_v3v3(r1, p1)); */
  const mpq3 &n1 = tri1.plane.norm_exact;
  int sp2 = sgn(mpq3::dot(p2 - r1, n1));
  int sq2 = sgn(mpq3::dot(q2 - r1, n1));
  int sr2 = sgn(mpq3::dot(r2 - r1, n1));

  if (dbg_level > 1) {
    std::cout << "  sp2=" << sp2 << " sq2=" << sq2 << " sr2=" << sr2 << "\n";
  }

  if ((sp2 * sq2 > 0) && (sp2 * sr2 > 0)) {
    if (dbg_level > 0) {
      std::cout << "no intersection, all t2's verts above or below t1\n";
    }
#ifdef PERFDEBUG
    incperfcount(2);
#endif
    return ITT_value(INONE);
  }

  /* Do rest of the work with vertices in a canonical order, where p1 is on
   * postive side of plane and q1, r1 are not; similarly for p2.
   */
  ITT_value ans;
  if (sp1 > 0) {
    if (sq1 > 0) {
      ans = itt_canon1(r1, p1, q1, p2, r2, q2, n1, n2, sp2, sr2, sq2);
    }
    else if (sr1 > 0) {
      ans = itt_canon1(q1, r1, p1, p2, r2, q2, n1, n2, sp2, sr2, sq2);
    }
    else {
      ans = itt_canon1(p1, q1, r1, p2, q2, r2, n1, n2, sp2, sq2, sr2);
    }
  }
  else if (sp1 < 0) {
    if (sq1 < 0) {
      ans = itt_canon1(r1, p1, q1, p2, q2, r2, n1, n2, sp2, sq2, sr2);
    }
    else if (sr1 < 0) {
      ans = itt_canon1(q1, r1, p1, p2, q2, r2, n1, n2, sp2, sq2, sr2);
    }
    else {
      ans = itt_canon1(p1, q1, r1, p2, r2, q2, n1, n2, sp2, sr2, sq2);
    }
  }
  else {
    if (sq1 < 0) {
      if (sr1 >= 0) {
        ans = itt_canon1(q1, r1, p1, p2, r2, q2, n1, n2, sp2, sr2, sq2);
      }
      else {
        ans = itt_canon1(p1, q1, r1, p2, q2, r2, n1, n2, sp2, sq2, sr2);
      }
    }
    else if (sq1 > 0) {
      if (sr1 > 0) {
        ans = itt_canon1(p1, q1, r1, p2, r2, q2, n1, n2, sp2, sr2, sq2);
      }
      else {
        ans = itt_canon1(q1, r1, p1, p2, q2, r2, n1, n2, sp2, sq2, sr2);
      }
    }
    else {
      if (sr1 > 0) {
        ans = itt_canon1(r1, p1, q1, p2, q2, r2, n1, n2, sp2, sq2, sr2);
      }
      else if (sr1 < 0) {
        ans = itt_canon1(r1, p1, q1, p2, r2, q2, n1, n2, sp2, sr2, sq2);
      }
      else {
        if (dbg_level > 0) {
          std::cout << "triangles are coplanar\n";
        }
        ans = ITT_value(ICOPLANAR);
      }
    }
  }
  if (ans.kind == ICOPLANAR) {
    ans.t_source = t2;
  }

#ifdef PERFDEBUG
  if (ans.kind != INONE) {
    incperfcount(5);
  }
#endif
  return ans;
}

struct CDT_data {
  Plane t_plane;
  Vector<mpq2> vert;
  Vector<std::pair<int, int>> edge;
  Vector<Vector<int>> face;
  Vector<int> input_face;        /* Parallels face, gives id from input Mesh of input face. */
  Vector<bool> is_reversed;      /* Parallels face, says if input face orientation is opposite. */
  CDT_result<mpq_class> cdt_out; /* Result of running CDT on input with (vert, edge, face). */
  int proj_axis;
};

/* We could dedup verts here, but CDT routine will do that anyway. */
static int prepare_need_vert(CDT_data &cd, const mpq3 &p3d)
{
  mpq2 p2d = project_3d_to_2d(p3d, cd.proj_axis);
  int v = cd.vert.append_and_get_index(p2d);
  return v;
}

/* To unproject a 2d vert that was projected along cd.proj_axis, we copy the coordinates
 * from the two axes not involved in the projection, and use the plane equation of the
 * originating 3d plane, cd.t_plane, to derive the coordinate of the projected axis.
 * The plane equation says a point p is on the plane if dot(p, plane.n()) + plane.d() == 0.
 * Assume that the projection axis is such that plane.n()[proj_axis] != 0.
 */
static mpq3 unproject_cdt_vert(const CDT_data &cd, const mpq2 &p2d)
{
  mpq3 p3d;
  BLI_assert(cd.t_plane.norm_exact[cd.proj_axis] != 0);
  const mpq3 &n = cd.t_plane.norm_exact;
  const mpq_class &d = cd.t_plane.d_exact;
  switch (cd.proj_axis) {
    case (0): {
      mpq_class num = n[1] * p2d[0] + n[2] * p2d[1] + d;
      num = -num;
      p3d[0] = num / n[0];
      p3d[1] = p2d[0];
      p3d[2] = p2d[1];
    } break;
    case (1): {
      p3d[0] = p2d[0];
      mpq_class num = n[0] * p2d[0] + n[2] * p2d[1] + d;
      num = -num;
      p3d[1] = num / n[1];
      p3d[2] = p2d[1];
    } break;
    case (2): {
      p3d[0] = p2d[0];
      p3d[1] = p2d[1];
      mpq_class num = n[0] * p2d[0] + n[1] * p2d[1] + d;
      num = -num;
      p3d[2] = num / n[2];
    } break;
    default:
      BLI_assert(false);
  }
  return p3d;
}

static void prepare_need_edge(CDT_data &cd, const mpq3 &p1, const mpq3 &p2)
{
  int v1 = prepare_need_vert(cd, p1);
  int v2 = prepare_need_vert(cd, p2);
  cd.edge.append(std::pair<int, int>(v1, v2));
}

static void prepare_need_tri(CDT_data &cd, const Mesh &tm, uint t)
{
  const Face &tri = *tm.face(t);
  int v0 = prepare_need_vert(cd, tri[0]->co_exact);
  int v1 = prepare_need_vert(cd, tri[1]->co_exact);
  int v2 = prepare_need_vert(cd, tri[2]->co_exact);
  bool rev;
  /* How to get CCW orientation of projected tri? Note that when look down y axis
   * as opposed to x or z, the orientation of the other two axes is not right-and-up.
   */
  if (cd.t_plane.norm_exact[cd.proj_axis] >= 0) {
    rev = cd.proj_axis == 1;
  }
  else {
    rev = cd.proj_axis != 1;
  }
  /* If t's plane is opposite to cd.t_plane, need to reverse again. */
  if (sgn(tri.plane.norm_exact[cd.proj_axis]) != sgn(cd.t_plane.norm_exact[cd.proj_axis])) {
    rev = !rev;
  }
  int cd_t = cd.face.append_and_get_index(Vector<int>());
  cd.face[cd_t].append(v0);
  if (rev) {
    cd.face[cd_t].append(v2);
    cd.face[cd_t].append(v1);
  }
  else {
    cd.face[cd_t].append(v1);
    cd.face[cd_t].append(v2);
  }
  cd.input_face.append(t);
  cd.is_reversed.append(rev);
}

static CDT_data prepare_cdt_input(const Mesh &tm, uint t, const Vector<ITT_value> itts)
{
  CDT_data ans;
  ans.t_plane = tm.face(t)->plane;
  const Plane &pl = ans.t_plane;
  ans.proj_axis = mpq3::dominant_axis(pl.norm_exact);
  prepare_need_tri(ans, tm, t);
  for (const ITT_value &itt : itts) {
    switch (itt.kind) {
      case INONE:
        break;
      case IPOINT: {
        prepare_need_vert(ans, itt.p1);
      } break;
      case ISEGMENT: {
        prepare_need_edge(ans, itt.p1, itt.p2);
      } break;
      case ICOPLANAR: {
        prepare_need_tri(ans, tm, itt.t_source);
      } break;
    }
  }
  return ans;
}

static CDT_data prepare_cdt_input_for_cluster(const Mesh &tm,
                                              const CoplanarClusterInfo &clinfo,
                                              uint c,
                                              const Vector<ITT_value> itts)
{
  CDT_data ans;
  BLI_assert(c < clinfo.tot_cluster());
  const CoplanarCluster &cl = clinfo.cluster(c);
  BLI_assert(cl.tot_tri() > 0);
  int t0 = cl.tri(0);
  ans.t_plane = tm.face(t0)->plane;
  const Plane &pl = ans.t_plane;
  ans.proj_axis = mpq3::dominant_axis(pl.norm_exact);
  for (const uint t : cl) {
    prepare_need_tri(ans, tm, t);
  }
  for (const ITT_value &itt : itts) {
    switch (itt.kind) {
      case IPOINT: {
        prepare_need_vert(ans, itt.p1);
      } break;
      case ISEGMENT: {
        prepare_need_edge(ans, itt.p1, itt.p2);
      } break;
      default:
        break;
    }
  }
  return ans;
}

/* Fills in cd.cdt_out with result of doing the cdt calculation on (vert, edge, face). */
static void do_cdt(CDT_data &cd)
{
  constexpr int dbg_level = 0;
  CDT_input<mpq_class> cdt_in;
  cdt_in.vert = Span<mpq2>(cd.vert);
  cdt_in.edge = Span<std::pair<int, int>>(cd.edge);
  cdt_in.face = Span<Vector<int>>(cd.face);
  if (dbg_level > 0) {
    std::cout << "CDT input\nVerts:\n";
    for (uint i = 0; i < cdt_in.vert.size(); ++i) {
      std::cout << "v" << i << ": " << cdt_in.vert[i] << "\n";
    }
    std::cout << "Edges:\n";
    for (uint i = 0; i < cdt_in.edge.size(); ++i) {
      std::cout << "e" << i << ": (" << cdt_in.edge[i].first << ", " << cdt_in.edge[i].second
                << ")\n";
    }
    std::cout << "Tris\n";
    for (uint f = 0; f < cdt_in.face.size(); ++f) {
      std::cout << "f" << f << ": ";
      for (uint j = 0; j < cdt_in.face[f].size(); ++j) {
        std::cout << cdt_in.face[f][j] << " ";
      }
      std::cout << "\n";
    }
  }
  cdt_in.epsilon = 0; /* TODO: needs attention for non-exact T. */
  cd.cdt_out = blender::meshintersect::delaunay_2d_calc(cdt_in, CDT_INSIDE);
  if (dbg_level > 0) {
    std::cout << "\nCDT result\nVerts:\n";
    for (uint i = 0; i < cd.cdt_out.vert.size(); ++i) {
      std::cout << "v" << i << ": " << cd.cdt_out.vert[i] << "\n";
    }
    std::cout << "Tris\n";
    for (uint f = 0; f < cd.cdt_out.face.size(); ++f) {
      std::cout << "f" << f << ": ";
      for (uint j = 0; j < cd.cdt_out.face[f].size(); ++j) {
        std::cout << cd.cdt_out.face[f][j] << " ";
      }
      std::cout << "orig: ";
      for (uint j = 0; j < cd.cdt_out.face_orig[f].size(); ++j) {
        std::cout << cd.cdt_out.face_orig[f][j] << " ";
      }
      std::cout << "\n";
    }
    std::cout << "Edges\n";
    for (uint e = 0; e < cd.cdt_out.edge.size(); ++e) {
      std::cout << "e" << e << ": (" << cd.cdt_out.edge[e].first << ", "
                << cd.cdt_out.edge[e].second << ") ";
      std::cout << "orig: ";
      for (uint j = 0; j < cd.cdt_out.edge_orig[e].size(); ++j) {
        std::cout << cd.cdt_out.edge_orig[e][j] << " ";
      }
      std::cout << "\n";
    }
  }
}

static int get_cdt_edge_orig(int i0, int i1, const CDT_data &cd, const Mesh &in_tm)
{
  int foff = cd.cdt_out.face_edge_offset;
  for (uint e : cd.cdt_out.edge.index_range()) {
    std::pair<int, int> edge = cd.cdt_out.edge[e];
    if ((edge.first == i0 && edge.second == i1) || (edge.first == i1 && edge.second == i0)) {
      /* Pick an arbitrary orig, but not one equal to NO_INDEX, if we can help it. */
      for (int orig_index : cd.cdt_out.edge_orig[e]) {
        /* orig_index encodes the triangle and pos within the triangle of the input edge. */
        if (orig_index >= foff) {
          int in_face_index = (orig_index / foff) - 1;
          int pos = orig_index % foff;
          /* We need to retrieve the edge orig field from the Face used to populate the
           * in_face_index'th face of the CDT, at the pos'th position of the face.
           */
          int in_tm_face_index = cd.input_face[in_face_index];
          BLI_assert(in_tm_face_index < in_tm.face_size());
          Facep facep = in_tm.face(in_tm_face_index);
          BLI_assert(pos < facep->size());
          bool is_rev = cd.is_reversed[in_face_index];
          int eorig = is_rev ? facep->edge_orig[2 - pos] : facep->edge_orig[pos];
          if (eorig != NO_INDEX) {
            return eorig;
          }
        }
        else {
          /* TODO: figure out how to track orig_index from an edge input to cdt. */
          /* This only matters if an input edge was formed by an input face having
           * an edge that is coplanar with the cluster, while the face as a whole is not.
           */
          return NO_INDEX;
        }
      }
      return NO_INDEX;
    }
  }
  return NO_INDEX;
}

/* Using the result of CDT in cd.cdt_out, extract a Mesh representing the subdivision
 * of input triangle t, which should be an element of cd.input_face.
 */
static Mesh extract_subdivided_tri(const CDT_data &cd, const Mesh &in_tm, uint t, MArena *arena)
{
  const CDT_result<mpq_class> &cdt_out = cd.cdt_out;
  int t_in_cdt = -1;
  for (int i = 0; i < static_cast<int>(cd.input_face.size()); ++i) {
    if (cd.input_face[i] == t) {
      t_in_cdt = i;
    }
  }
  if (t_in_cdt == -1) {
    std::cout << "Could not find " << t << " in cdt input tris\n";
    BLI_assert(false);
    return Mesh();
  }
  int t_orig = in_tm.face(t)->orig;
  constexpr int inline_buf_size = 20;
  Vector<Facep, inline_buf_size> faces;
  for (uint f = 0; f < cdt_out.face.size(); ++f) {
    if (cdt_out.face_orig[f].contains(t_in_cdt)) {
      BLI_assert(cdt_out.face[f].size() == 3);
      int i0 = cdt_out.face[f][0];
      int i1 = cdt_out.face[f][1];
      int i2 = cdt_out.face[f][2];
      mpq3 v0co = unproject_cdt_vert(cd, cdt_out.vert[i0]);
      mpq3 v1co = unproject_cdt_vert(cd, cdt_out.vert[i1]);
      mpq3 v2co = unproject_cdt_vert(cd, cdt_out.vert[i2]);
      /* No need to provide an original index: if coord matches
       * an original one, then it will already be in the arena
       * with the correct orig field.
       */
      Vertp v0 = arena->add_or_find_vert(v0co, NO_INDEX);
      Vertp v1 = arena->add_or_find_vert(v1co, NO_INDEX);
      Vertp v2 = arena->add_or_find_vert(v2co, NO_INDEX);
      Facep facep;
      if (cd.is_reversed[t_in_cdt]) {
        int oe0 = get_cdt_edge_orig(i0, i2, cd, in_tm);
        int oe1 = get_cdt_edge_orig(i2, i1, cd, in_tm);
        int oe2 = get_cdt_edge_orig(i1, i0, cd, in_tm);
        facep = arena->add_face({v0, v2, v1}, t_orig, {oe0, oe1, oe2});
      }
      else {
        int oe0 = get_cdt_edge_orig(i0, i1, cd, in_tm);
        int oe1 = get_cdt_edge_orig(i1, i2, cd, in_tm);
        int oe2 = get_cdt_edge_orig(i2, i0, cd, in_tm);
        facep = arena->add_face({v0, v1, v2}, t_orig, {oe0, oe1, oe2});
      }
      faces.append(facep);
    }
  }
  return Mesh(faces);
}

static Mesh extract_single_tri(const Mesh &tm, uint t)
{
  Facep f = tm.face(t);
  return Mesh({f});
}

static bool bvhtreeverlap_cmp(const BVHTreeOverlap &a, const BVHTreeOverlap &b)
{
  if (a.indexA < b.indexA) {
    return true;
  }
  if (a.indexA == b.indexA & a.indexB < b.indexB) {
    return true;
  }
  return false;
}

/* For each triangle in tm, fill in the corresponding slot in
 * r_tri_subdivided with the result of intersecting it with
 * all the other triangles in the mesh, if it intersects any others.
 * But don't do this for triangles that are part of a cluster.
 * Also, do nothing here if the answer is just the triangle itself.
 * TODO: parallelize this loop.
 */
static void calc_subdivided_tris(Array<Mesh> &r_tri_subdivided,
                                 const Mesh &tm,
                                 const CoplanarClusterInfo &clinfo,
                                 BVHTree *tri_tree,
                                 MArena *arena)
{
  const int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "\nCALC_SUBDIVIDED_TRIS\n\n";
  }
  uint overlap_tot;
  BVHTreeOverlap *overlap = BLI_bvhtree_overlap(tri_tree, tri_tree, &overlap_tot, NULL, NULL);
  if (overlap == nullptr) {
    return;
  }
  if (overlap_tot <= 1) {
    MEM_freeN(overlap);
    return;
  }
  /* Sort the overlaps to bring all the intersects with a given indexA together.   */
  std::sort(overlap, overlap + overlap_tot, bvhtreeverlap_cmp);
  uint overlap_index = 0;
  while (overlap_index < overlap_tot) {
    int t = overlap[overlap_index].indexA;
    uint i = overlap_index;
    while (i + 1 < overlap_tot && overlap[i + 1].indexA == t) {
      ++i;
    }
    /* Now overlap[overlap_index] to overlap[i] have indexA == t. */
    if (clinfo.tri_cluster(t) != NO_INDEX_U) {
      /* Triangles in clusters are handled separately. */
      overlap_index = i + 1;
      continue;
    }
    if (dbg_level > 0) {
      std::cout << "tri t" << t << " maybe intersects with:\n";
    }
    constexpr int inline_capacity = 100;
    Vector<ITT_value, inline_capacity> itts;
    uint tu = static_cast<uint>(t);
    for (uint j = overlap_index; j <= i; ++j) {
      uint t_other = static_cast<uint>(overlap[j].indexB);
      if (t_other == tu) {
        continue;
      }
#ifdef PERFDEBUG
      incperfcount(3);
#endif
      ITT_value itt;
      if (may_non_trivially_intersect(tm.face(tu), tm.face(t_other))) {
        itt = intersect_tri_tri(tm, tu, t_other);
      }
      else {
        if (dbg_level > 0) {
          std::cout << "early discovery of only trivial intersect\n";
        }
#ifdef PERFDEBUG
        incperfcount(4);
#endif
        itt = ITT_value(INONE);
      }
      if (itt.kind != INONE) {
        itts.append(itt);
      }
      if (dbg_level > 0) {
        std::cout << "  tri t" << t_other << "; result = " << itt << "\n";
      }
    }
    if (itts.size() > 0) {
      CDT_data cd_data = prepare_cdt_input(tm, tu, itts);
      do_cdt(cd_data);
      r_tri_subdivided[tu] = extract_subdivided_tri(cd_data, tm, tu, arena);
    }
    overlap_index = i + 1;
  }
  MEM_freeN(overlap);
}

static CDT_data calc_cluster_subdivided(const CoplanarClusterInfo &clinfo,
                                        uint c,
                                        const Mesh &tm,
                                        MArena *UNUSED(arena))
{
  constexpr int dbg_level = 0;
  BLI_assert(c < clinfo.tot_cluster());
  const CoplanarCluster &cl = clinfo.cluster(c);
  /* Make a CDT input with triangles from C and intersects from other triangles in tm. */
  if (dbg_level > 0) {
    std::cout << "calc_cluster_subdivided for cluster " << c << " = " << cl << "\n";
  }
  /* Get vector itts of all intersections of a triangle of cl with any triangle of tm not
   * in cl and not coplanar with it (for that latter, if there were an intersection,
   * it should already be in cluster cl).
   */
  Vector<ITT_value> itts;
  for (uint t_other : tm.face_index_range()) {
    if (clinfo.tri_cluster(t_other) != c) {
      if (dbg_level > 0) {
        std::cout << "intersect cluster " << c << " with tri " << t_other << "\n";
      }
      for (const uint t : cl) {
        ITT_value itt = intersect_tri_tri(tm, t, t_other);
        if (dbg_level > 0) {
          std::cout << "intersect tri " << t << " with tri " << t_other << " = " << itt << "\n";
        }
        if (itt.kind != INONE && itt.kind != ICOPLANAR) {
          itts.append(itt);
        }
      }
    }
  }
  /* Use CDT to subdivide the cluster triangles and the points and segs in itts. */
  CDT_data cd_data = prepare_cdt_input_for_cluster(tm, clinfo, c, itts);
  do_cdt(cd_data);
  return cd_data;
}

static Mesh union_tri_subdivides(const blender::Array<Mesh> &tri_subdivided)
{
  uint tot_tri = 0;
  for (const Mesh &m : tri_subdivided) {
    tot_tri += m.face_size();
  }
  Array<Facep> faces(tot_tri);
  uint face_index = 0;
  for (const Mesh &m : tri_subdivided) {
    for (Facep f : m.faces()) {
      faces[face_index++] = f;
    }
  }
  return Mesh(faces);
}

static CoplanarClusterInfo find_clusters(const Mesh &tm, const Array<BoundingBox> &tri_bb)
{
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "FIND_CLUSTERS\n";
  }
  CoplanarClusterInfo ans(tm.face_size());
  /* There can be more than one CoplanarCluster per plane. Accumulate them in
   * a Vector. We will have to merge some elements of the Vector as we discover
   * triangles that form intersection bridges between two or more clusters.
   */
  Map<Plane, Vector<CoplanarCluster>> plane_cls;
  plane_cls.reserve(tm.face_size());
  for (uint t : tm.face_index_range()) {
    /* Use a canonical version of the plane for map index.
     * We can't just store the canonical version in the face
     * since canonicalizing loses the orientation of the normal.
     */
    Plane tplane = tm.face(t)->plane;
    tplane.make_canonical();
    if (dbg_level > 0) {
      std::cout << "plane for tri " << t << " = " << tplane << "\n";
    }
    /* Assume all planes are in canonical from (see canon_plane()). */
    if (plane_cls.contains(tplane)) {
      Vector<CoplanarCluster> &curcls = plane_cls.lookup(tplane);
      if (dbg_level > 0) {
        std::cout << "already has " << curcls.size() << " clusters\n";
      }
      int proj_axis = mpq3::dominant_axis(tplane.norm_exact);
      /* Paritition curcls into those that intersect t non-trivially, and those that don't. */
      Vector<CoplanarCluster *> int_cls;
      Vector<CoplanarCluster *> no_int_cls;
      for (CoplanarCluster &cl : curcls) {
        if (bbs_might_intersect(tri_bb[t], cl.bounding_box()) &&
            non_trivially_coplanar_intersects(tm, t, cl, proj_axis)) {
          int_cls.append(&cl);
        }
        else {
          no_int_cls.append(&cl);
        }
      }
      if (int_cls.size() == 0) {
        /* t doesn't intersect any existing cluster in its plane, so make one just for it. */
        curcls.append(CoplanarCluster(t, tri_bb[t]));
      }
      else if (int_cls.size() == 1) {
        /* t intersects exactly one existing cluster, so can add t to that cluster. */
        int_cls[0]->add_tri(t, tri_bb[t]);
      }
      else {
        /* t intersections 2 or more existing clusters: need to merge them and replace all the
         * originals with the merged one in curcls.
         */
        CoplanarCluster mergecl;
        mergecl.add_tri(t, tri_bb[t]);
        for (CoplanarCluster *cl : int_cls) {
          for (uint t : *cl) {
            mergecl.add_tri(t, tri_bb[t]);
          }
        }
        Vector<CoplanarCluster> newvec;
        newvec.append(mergecl);
        for (CoplanarCluster *cl_no_int : no_int_cls) {
          newvec.append(*cl_no_int);
        }
        plane_cls.add_overwrite(tplane, newvec);
      }
    }
    else {
      if (dbg_level > 0) {
        std::cout << "first cluster for its plane\n";
      }
      plane_cls.add_new(tplane, Vector<CoplanarCluster>{CoplanarCluster(t, tri_bb[t])});
    }
  }
  /* Does this give deterministic order for cluster ids? I think so, since
   * hash for planes is on their values, not their addresses.
   */
  for (auto item : plane_cls.items()) {
    for (const CoplanarCluster &cl : item.value) {
      if (cl.tot_tri() > 1) {
        ans.add_cluster(cl);
      }
    }
  }

  return ans;
}

/* Does TriMesh tm have any triangles with zero area? */
static bool has_degenerate_tris(const Mesh &tm)
{
  for (Facep f : tm.faces()) {
    const Face &face = *f;
    Vertp v0 = face[0];
    Vertp v1 = face[1];
    Vertp v2 = face[2];
    if (v0 == v1 || v0 == v2 || v1 == v2) {
      return true;
    }
    mpq3 a = v2->co_exact - v0->co_exact;
    mpq3 b = v2->co_exact - v1->co_exact;
    mpq3 ab = mpq3::cross(a, b);
    if (ab.x == 0 && ab.y == 0 && ab.z == 0) {
      return true;
    }
  }
  return false;
}

/* Caller will be responsible for doing BLI_bvhtree_free on return val. */
static BVHTree *bvhtree_for_tris(const Mesh &tm, const Array<BoundingBox> &tri_bb)
{
  /* Tree type is 8 => octtree; axis = 6 => using XYZ axes only. */
  BVHTree *tri_tree = BLI_bvhtree_new(static_cast<int>(tm.face_size()), FLT_EPSILON, 8, 6);
  float bbpts[6];
  for (uint t : tm.face_index_range()) {
    const BoundingBox &bb = tri_bb[t];
    copy_v3_v3(bbpts, bb.min);
    copy_v3_v3(bbpts + 3, bb.max);
    BLI_bvhtree_insert(tri_tree, static_cast<int>(t), bbpts, 2);
  }
  BLI_bvhtree_balance(tri_tree);
  return tri_tree;
}

/* Maybe return nullptr if there are no clsuters.
 * If not, caller is responsible for doing BLI_bvhtree_free on return val.
 */
static BVHTree *bvhtree_for_clusters(const CoplanarClusterInfo &clinfo)
{
  int nc = static_cast<int>(clinfo.tot_cluster());
  if (nc == 0) {
    return nullptr;
  }
  BVHTree *cluster_tree = BLI_bvhtree_new(nc, FLT_EPSILON, 8, 6);
  float bbpts[6];
  for (uint c : clinfo.index_range()) {
    const BoundingBox &bb = clinfo.cluster(c).bounding_box();
    copy_v3_v3(bbpts, bb.min);
    copy_v3_v3(bbpts + 3, bb.max);
    BLI_bvhtree_insert(cluster_tree, static_cast<int>(c), bbpts, 2);
  }
  BLI_bvhtree_balance(cluster_tree);
  return cluster_tree;
}

/* This is the main routine for calculating the self_intersection of a triangle mesh. */
Mesh trimesh_self_intersect(const Mesh &tm_in, MArena *arena)
{
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "\nTRIMESH_SELF_INTERSECT\n";
    for (Facep f : tm_in.faces()) {
      BLI_assert(f->is_tri());
    }
  }
  if (has_degenerate_tris(tm_in)) {
    std::cout << "IMPLEMENT ME - remove degenerate and illegal tris\n";
    BLI_assert(false);
  }
  Array<BoundingBox> tri_bb = calc_face_bounding_boxes(tm_in);
  /* Clusters have at least two coplanar, non-trivially intersecting triangles. */
  CoplanarClusterInfo clinfo = find_clusters(tm_in, tri_bb);
  if (dbg_level > 1) {
    std::cout << clinfo;
  }
#ifdef PERFDEBUG
  perfdata_init();
  doperfmax(0, static_cast<int>(tm_in.face_size()));
  doperfmax(1, static_cast<int>(clinfo.tot_cluster()));
#endif
  BVHTree *tri_tree = bvhtree_for_tris(tm_in, tri_bb);
  BVHTree *cluster_tree = bvhtree_for_clusters(clinfo);
  Array<CDT_data> cluster_subdivided(clinfo.tot_cluster());
  for (uint c : clinfo.index_range()) {
    cluster_subdivided[c] = calc_cluster_subdivided(clinfo, c, tm_in, arena);
  }
  blender::Array<Mesh> tri_subdivided(tm_in.face_size());
  calc_subdivided_tris(tri_subdivided, tm_in, clinfo, tri_tree, arena);
  for (uint t : tm_in.face_index_range()) {
    uint c = clinfo.tri_cluster(t);
    if (c != NO_INDEX_U) {
      BLI_assert(tri_subdivided[t].face_size() == 0);
      tri_subdivided[t] = extract_subdivided_tri(cluster_subdivided[c], tm_in, t, arena);
    }
    else if (tri_subdivided[t].face_size() == 0) {
      tri_subdivided[t] = extract_single_tri(tm_in, t);
    }
  }
  Mesh combined = union_tri_subdivides(tri_subdivided);
  if (dbg_level > 1) {
    std::cout << "TRIMESH_SELF_INTERSECT answer:\n";
    std::cout << combined;
  }
  BLI_bvhtree_free(tri_tree);
  if (cluster_tree != nullptr) {
    BLI_bvhtree_free(cluster_tree);
  }
#ifdef PERFDEBUG
  dump_perfdata();
#endif
  return combined;
}

static std::ostream &operator<<(std::ostream &os, const CoplanarCluster &cl)
{
  os << "cl(";
  bool first = true;
  for (const uint t : cl) {
    if (first) {
      first = false;
    }
    else {
      os << ",";
    }
    os << t;
  }
  os << ")";
  return os;
}

static std::ostream &operator<<(std::ostream &os, const CoplanarClusterInfo &clinfo)
{
  os << "Coplanar Cluster Info:\n";
  for (uint c : clinfo.index_range()) {
    os << c << ": " << clinfo.cluster(c) << "\n";
  }
  return os;
}

static std::ostream &operator<<(std::ostream &os, const ITT_value &itt)
{
  switch (itt.kind) {
    case INONE:
      os << "none";
      break;
    case IPOINT:
      os << "point " << itt.p1;
      break;
    case ISEGMENT:
      os << "segment " << itt.p1 << " " << itt.p2;
      break;
    case ICOPLANAR:
      os << "coplanar t" << itt.t_source;
      break;
  }
  return os;
}

/* Writing the obj_mesh has the side effect of populating verts. */
void write_obj_mesh(Mesh &m, const std::string &objname)
{
  constexpr const char *objdir = "/tmp/";
  if (m.face_size() == 0) {
    return;
  }

  std::string fname = std::string(objdir) + objname + std::string(".obj");
  std::ofstream f;
  f.open(fname);
  if (!f) {
    std::cout << "Could not open file " << fname << "\n";
    return;
  }

  if (!m.has_verts()) {
    m.populate_vert();
  }
  for (Vertp v : m.vertices()) {
    const double3 dv = v->co;
    f << "v " << dv[0] << " " << dv[1] << " " << dv[2] << "\n";
  }
  int i = 0;
  for (Facep face : m.faces()) {
    /* OBJ files use 1-indexing for vertices. */
    f << "f ";
    for (Vertp v : *face) {
      int i = m.lookup_vert(v);
      BLI_assert(i != NO_INDEX);
      /* OBJ files use 1-indexing for vertices. */
      f << i + 1 << " ";
    }
    f << "\n";
    ++i;
  }
  f.close();
}

#ifdef PERFDEBUG
struct PerfCounts {
  Vector<int> count;
  Vector<const char *> count_name;
  Vector<int> max;
  Vector<const char *> max_name;
} perfdata;

static void perfdata_init(void)
{
  /* count 0. */
  perfdata.count.append(0);
  perfdata.count_name.append("intersect_tri_tri calls");

  /* count 1. */
  perfdata.count.append(0);
  perfdata.count_name.append("trivial intersects detected post intersect_tri_tri");

  /* count 2. */
  perfdata.count.append(0);
  perfdata.count_name.append("tri tri intersects stopped by plane tests");

  /* count 3. */
  perfdata.count.append(0);
  perfdata.count_name.append("overlaps");

  /* count 4. */
  perfdata.count.append(0);
  perfdata.count_name.append("early discovery of trivial intersects");

  /* count 5. */
  perfdata.count.append(0);
  perfdata.count_name.append("final non-NONE intersects");

  /* max 0. */
  perfdata.max.append(0);
  perfdata.max_name.append("total faces");

  /* max 1. */
  perfdata.max.append(0);
  perfdata.max_name.append("total clusters");
}

static void incperfcount(int countnum)
{
  perfdata.count[countnum]++;
}

static void doperfmax(int maxnum, int val)
{
  perfdata.max[maxnum] = max_ii(perfdata.max[maxnum], val);
}

static void dump_perfdata(void)
{
  std::cout << "\nPERFDATA\n";
  for (uint i : perfdata.count.index_range()) {
    std::cout << perfdata.count_name[i] << " = " << perfdata.count[i] << "\n";
  }
  for (uint i : perfdata.max.index_range()) {
    std::cout << perfdata.max_name[i] << " = " << perfdata.max[i] << "\n";
  }
}
#endif

};  // namespace blender::meshintersect
