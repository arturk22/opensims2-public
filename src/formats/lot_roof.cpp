// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Port of OpenTS2's roof geometry (Assets/Scripts/OpenTS2/Scenes/Lot/Roof/:
// RoofCollection, AbstractSimpleRoof and the roof types, RoofEdge, RoofTile;
// MPL-2.0): https://github.com/LazyDuchess/OpenTS2. Changes: Z-up lot space
// instead of Unity's Y-up, counter-clockwise front faces, normals.
#include "lot_roof.hpp"

#include "lot.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace os2 {

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr uint32_t kDefaultPattern = 0x0CDCC049;

struct V2 {
    float x = 0, y = 0;
    V2 operator+(V2 o) const { return {x + o.x, y + o.y}; }
    V2 operator-(V2 o) const { return {x - o.x, y - o.y}; }
    V2 operator*(float s) const { return {x * s, y * s}; }
    float length() const { return std::sqrt(x * x + y * y); }
    float lengthSq() const { return x * x + y * y; }
    V2 normalized() const {
        const float l = length();
        return l > 1e-5f ? V2{x / l, y / l} : V2{};
    }
};
float dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
// Unsigned angle in degrees (Unity's Vector2.Angle).
float angleDeg(V2 a, V2 b) {
    const float d = std::sqrt(a.lengthSq() * b.lengthSq());
    if (d < 1e-15f)
        return 0;
    return std::acos(std::clamp(dot(a, b) / d, -1.0f, 1.0f)) * 57.29578f;
}
int roundToInt(float v) { return int(std::nearbyint(v)); } // half to even, like Unity's Mathf.RoundToInt

struct V3 {
    float x = 0, y = 0, z = 0;
    V3 operator+(V3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(V3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(float s) const { return {x * s, y * s, z * s}; }
};

// --- RoofTile ---

enum class TileBase : uint8_t { Normal, LeftEdge, SmallLeftEdge, LeftToTop, Top, TopToRight, SmallRightEdge, RightEdge, Empty };
enum class TileIntersection : uint8_t { None, SmallLeft, SmallRight, Bottom, Left, Right };
enum Cut : uint8_t { CutTop = 1, CutRight = 2, CutBottom = 4, CutLeft = 8, CutAll = 15 };
constexpr uint8_t CutTopRight = CutTop | CutRight, CutTopLeft = CutTop | CutLeft, CutBottomRight = CutBottom | CutRight,
                  CutBottomLeft = CutBottom | CutLeft;

struct AtlasIndex {
    int index;
    bool flip = false;
};

// [base][intersection] -> cell of the 5 x 5 atlas.
const AtlasIndex kAtlas[9][6] = {
    {{9}, {15, true}, {15}, {13}, {2}, {16}},             // Normal
    {{20}, {-1}, {-1}, {12}, {-1}, {-1}},                 // LeftEdge
    {{21}, {6}, {-1}, {17}, {-1}, {10}},                  // SmallLeftEdge
    {{8}, {7}, {-1}, {3}, {11, true}, {11}},              // LeftToTop
    {{22}, {18, true}, {18}, {23}, {11, true}, {11}},     // Top
    {{8, true}, {-1}, {7, true}, {3, true}, {11, true}, {11}}, // TopToRight
    {{14}, {-1}, {6, true}, {17, true}, {10, true}, {-1}}, // SmallRightEdge
    {{1}, {-1}, {-1}, {12, true}, {-1}, {-1}},            // RightEdge
    {{-1}, {-1}, {-1}, {-1}, {-1}, {-1}},                 // Empty
};

uint8_t baseCut(TileBase b) {
    switch (b) {
    case TileBase::LeftEdge: return CutTopLeft;
    case TileBase::RightEdge: return CutTopRight;
    case TileBase::Empty: return CutAll;
    default: return 0;
    }
}

uint8_t rotateCut(uint8_t cut, int dir) {
    if (dir == 0)
        return cut;
    return uint8_t(((cut >> dir) | (cut << (4 - dir))) & CutAll);
}

struct Tile {
    TileBase base = TileBase::Empty;
    TileIntersection intersection = TileIntersection::None;
    uint8_t cut = CutAll;

    Tile() = default;
    explicit Tile(TileBase b) : base(b), cut(baseCut(b)) {}

    void addIntersection(TileIntersection in) {
        if (in > intersection && kAtlas[int(base)][int(in)].index != -1)
            intersection = in;
    }
    void erase() { cut = CutAll; }
    void cutWith(uint8_t other, int dir) { cut |= uint8_t(CutAll ^ rotateCut(other, dir)); }
    AtlasIndex atlas() const { return kAtlas[int(base)][int(intersection)]; }
    uint8_t ownBaseCut() const { return baseCut(base); }
    int overlapPriority() const {
        if (cut == CutAll)
            return -1;
        switch (base) {
        case TileBase::Normal: return 4;
        case TileBase::SmallLeftEdge:
        case TileBase::SmallRightEdge: return 3;
        case TileBase::Top: return 2;
        case TileBase::LeftToTop:
        case TileBase::TopToRight: return 1;
        default: return 0;
        }
    }
    void addIntersectionsFrom(const Tile& t) {
        if (t.intersection != TileIntersection::None)
            addIntersection(t.intersection);
    }
};

// --- Mesh output ---

struct Out {
    MeshGroup* mesh;
    uint32_t add(V3 p, std::array<float, 2> uv, V3 n) {
        mesh->positions.push_back({p.x, p.y, p.z});
        mesh->uvs.push_back(uv);
        mesh->normals.push_back({n.x, n.y, n.z});
        return uint32_t(mesh->positions.size() - 1);
    }
    // Front face counter-clockwise from the side the normal points to.
    void tri(uint32_t a, uint32_t b, uint32_t c) { mesh->triangles.insert(mesh->triangles.end(), {a, b, c}); }
};

V3 faceNormal(V3 a, V3 b, V3 c) {
    const V3 u = b - a, v = c - a;
    V3 n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    return l > 0 ? n * (1 / l) : V3{0, 0, 1};
}

enum class EdgeEnd { Normal, Flat, FlatShort };

} // namespace

// --- RoofEdge: one sloped face of a roof ---

class RoofEdge {
public:
    static constexpr float kThicknessTop = 0.075f, kThicknessInner = 0.15f,
                           kThicknessTotal = kThicknessTop + kThicknessInner, kTileSize = 0.5f,
                           kDiagonalScale = 0.7071067811865475f, kAtlasUVYOff = 2.0f / 512.0f,
                           kAtlasUVSize = 102.0f / 512.0f, kBias = 0.0001f;

    RoofEdge(float height, float slope, V2 bl, V2 br, V2 tr, V2 tl, EdgeEnd lEnd = EdgeEnd::Normal,
             EdgeEnd rEnd = EdgeEnd::Normal, int topOverhang = 0, bool pagoda = false)
        : height_(height), slope_(slope), bl_(bl), br_(br), tr_(tr), tl_(tl), lEnd_(lEnd), rEnd_(rEnd),
          topOverhang_(topOverhang), pagoda_(pagoda) {
        yNormal_ = tl - bl;
        xNormal_ = br - bl;
        const float xDist = xNormal_.length(), yDist = yNormal_.length();
        yNormal_ = yNormal_.normalized();
        xNormal_ = xNormal_.normalized();
        if (yNormal_.lengthSq() < 0.5f)
            yNormal_ = {-xNormal_.y, xNormal_.x};
        if (xNormal_.lengthSq() < 0.5f)
            xNormal_ = {yNormal_.y, -yNormal_.x};
        bPc_ = dot(yNormal_, bl);
        rPc_ = dot(xNormal_, br);
        tPc_ = dot(yNormal_, tr);
        lPc_ = dot(xNormal_, tl);

        // Only cardinal and 45 degree faces exist.
        diagonal_ = std::abs(xNormal_.x) < 0.9f && std::abs(xNormal_.y) < 0.9f;
        if (diagonal_) {
            tileSize_ = kDiagonalScale * kTileSize;
            slope_ *= kDiagonalScale;
            overhang_ = 2;
        } else {
            tileSize_ = kTileSize;
            overhang_ = 1;
        }
        if (pagoda)
            overhang_ += 2;
        tileWidth_ = roundToInt(xDist / tileSize_) + (lEnd_ == EdgeEnd::FlatShort ? 0 : overhang_) +
                     (rEnd_ == EdgeEnd::FlatShort ? 0 : overhang_);
        tileHeight_ = roundToInt(yDist / tileSize_) + overhang_;
        tiles_.resize(size_t(std::max(tileWidth_, 0) * std::max(tileHeight_, 0)));
        initTiles();
    }

    void generate(Out top, Out edges, Out trim, Out under) const {
        V3 base{tl_.x, tl_.y, height_ + maxHeight() + kThicknessTop};
        const V3 xTile{xNormal_.x * tileSize_, xNormal_.y * tileSize_, 0};
        V3 yTile{yNormal_.x * -tileSize_, yNormal_.y * -tileSize_, -slope_};
        if (lEnd_ != EdgeEnd::FlatShort)
            base = base - xTile * float(overhang_);

        size_t i = 0;
        float yBase = 0;
        for (int y = 0; y < tileHeight_; ++y) {
            if (pagoda_)
                yTile.z = -pagodaSlopeAt(y);
            for (int x = 0; x < tileWidth_; ++x, ++i) {
                const Tile& tile = tiles_[i];
                const AtlasIndex graphic = tile.atlas();
                if (graphic.index == -1)
                    continue;
                std::array<V3, 5> v;
                v[0] = base + xTile * float(x) + V3{yTile.x, yTile.y, 0} * float(y); // top left (ridge side)
                v[0].z += yBase;
                v[1] = v[0] + xTile; // top right
                v[2] = v[1] + yTile; // bottom right (eave side)
                v[3] = v[0] + yTile; // bottom left
                if (pagoda_) {
                    v[0].z += pagodaCornerOffset(float(x));
                    v[1].z += pagodaCornerOffset(float(x + 1));
                    v[2].z += pagodaCornerOffset(float(x + 1));
                    v[3].z += pagodaCornerOffset(float(x));
                }
                v[4] = (v[0] + v[1] + v[2] + v[3]) * 0.25f;

                const float bu = float(graphic.index % 5) * kAtlasUVSize,
                            bv = 1 - (float(graphic.index / 5) * kAtlasUVSize + kAtlasUVYOff);
                std::array<std::array<float, 2>, 5> uv{{{bu, bv - kAtlasUVSize},
                                                        {bu + kAtlasUVSize, bv - kAtlasUVSize},
                                                        {bu + kAtlasUVSize, bv},
                                                        {bu, bv},
                                                        {bu + kAtlasUVSize / 2, bv - kAtlasUVSize / 2}}};
                if (graphic.flip) {
                    std::swap(uv[0], uv[1]);
                    std::swap(uv[2], uv[3]);
                }
                // Faces wind clockwise from above in this order (Unity is
                // left-handed); emitted reversed for counter-clockwise fronts.
                const V3 n = faceNormal(v[0], v[2], v[1]);
                const V3 down{0, 0, -kThicknessTotal};
                const V3 nd = n * -1.0f;
                static const std::array<std::array<float, 2>, 5> underUV{{{0, 1}, {1, 1}, {1, 0}, {0, 0}, {0.5f, 0.5f}}};
                std::array<uint32_t, 5> t, u;
                for (int k = 0; k < 5; ++k) {
                    t[size_t(k)] = top.add(v[size_t(k)], uv[size_t(k)], n);
                    u[size_t(k)] = under.add(v[size_t(k)] + down, underUV[size_t(k)], nd);
                }
                const uint8_t cut = tile.cut;
                auto quarter = [&](int a, int b) {
                    top.tri(t[size_t(a)], t[4], t[size_t(b)]);
                    under.tri(u[size_t(b)], u[4], u[size_t(a)]);
                };
                if (cut == 0) {
                    top.tri(t[0], t[2], t[1]);
                    top.tri(t[2], t[0], t[3]);
                    under.tri(u[1], u[2], u[0]);
                    under.tri(u[3], u[0], u[2]);
                } else {
                    if (!(cut & CutTop))
                        quarter(0, 1);
                    if (!(cut & CutRight))
                        quarter(1, 2);
                    if (!(cut & CutBottom))
                        quarter(2, 3);
                    if (!(cut & CutLeft))
                        quarter(3, 0);
                }
                if (y == 0 && !(cut & CutTop))
                    drawTrim(trim, edges, v[1], v[0]);
                if (y == tileHeight_ - 1 && !(cut & CutBottom))
                    drawTrim(trim, edges, v[3], v[2]);
                if (lEnd_ != EdgeEnd::Normal && x == 0 && !(cut & CutLeft))
                    drawTrim(trim, edges, v[0], v[3]);
                if (rEnd_ != EdgeEnd::Normal && x == tileWidth_ - 1 && !(cut & CutRight))
                    drawTrim(trim, edges, v[2], v[1]);
            }
            yBase += yTile.z;
        }
    }

    float heightAt(float x, float y) const {
        const V2 pos{x, y};
        const float dx = dot(pos, xNormal_), dy = dot(pos, yNormal_);
        if (dx < lPc_ || dx > rPc_ || dy < bPc_ || dy > tPc_)
            return kInf;
        const float yDist = dy - bPc_;
        // Hipped ends also cut off the top 45 degrees at the left and right.
        if ((lEnd_ == EdgeEnd::Normal && dx - lPc_ < yDist) || (rEnd_ == EdgeEnd::Normal && rPc_ - dx < yDist))
            return kInf;
        if (pagoda_) {
            const float fromBottom = yDist / tileSize_ + float(overhang_);
            const float heightIntersect = float(tileHeight_ - (overhang_ + 2)) * slope_;
            const float intersectY = float(tileHeight_ - 2);
            const float a = heightIntersect / (intersectY * intersectY);
            return a * fromBottom * fromBottom + height_;
        }
        return yDist * (slope_ / tileSize_) + height_;
    }

    // Cuts the tiles of both faces where they meet; false when the faces'
    // tiles do not line up (then lower tiles are removed by height instead).
    bool intersect(RoofEdge& other);

    template <typename HeightFn>
    void removeTilesUnder(HeightFn&& otherHeight) {
        const V3 xTile{xNormal_.x * tileSize_, xNormal_.y * tileSize_, 0};
        const V3 yTile{yNormal_.x * -tileSize_, yNormal_.y * -tileSize_, -slope_};
        const V3 base = V3{tl_.x, tl_.y, height_ + float(tileHeight_ - 1) * slope_} - xTile;
        size_t i = 0;
        for (int y = 0; y < tileHeight_; ++y)
            for (int x = 0; x < tileWidth_; ++x, ++i) {
                const V3 tl = base + xTile * float(x) + yTile * float(y), tr = tl + xTile, br = tr + yTile,
                         bl = tl + yTile;
                auto below = [&](V3 p) {
                    const float h = otherHeight(p.x, p.y);
                    return h != kInf && p.z + kBias < h;
                };
                if (below(tl) && below(tr) && below(br) && below(bl))
                    tiles_[i].erase();
            }
    }

private:
    float height_, slope_;
    V2 bl_, br_, tr_, tl_, yNormal_, xNormal_;
    float bPc_ = 0, rPc_ = 0, tPc_ = 0, lPc_ = 0;
    EdgeEnd lEnd_, rEnd_;
    std::vector<Tile> tiles_;
    int tileWidth_ = 0, tileHeight_ = 0;
    float tileSize_ = kTileSize;
    int overhang_ = 1, topOverhang_ = 0;
    bool pagoda_ = false, diagonal_ = false;

    Tile& at(int x, int y) { return tiles_[size_t(y * tileWidth_ + x)]; }

    void initTiles() {
        int mid = 0;
        if (rEnd_ == EdgeEnd::Normal && lEnd_ == EdgeEnd::Normal)
            mid = tileWidth_ / 2;
        else if (lEnd_ == EdgeEnd::Normal)
            mid = tileWidth_;
        size_t i = 0;
        for (int y = 0; y < tileHeight_; ++y)
            for (int x = 0; x < tileWidth_; ++x, ++i) {
                Tile& tile = tiles_[i];
                const int side = x + y, oside = (tileWidth_ - 1 - x) + y;
                if (lEnd_ == EdgeEnd::Normal && x < mid && side <= tileHeight_) {
                    if (side == tileHeight_)
                        tile = Tile(y != 0 ? TileBase::SmallLeftEdge : TileBase::LeftToTop);
                    else if (side == tileHeight_ - 1)
                        tile = Tile(TileBase::LeftEdge);
                    else
                        tile = Tile(TileBase::Empty);
                    continue;
                }
                if (rEnd_ == EdgeEnd::Normal && x >= mid && oside <= tileHeight_) {
                    if (oside == tileHeight_)
                        tile = Tile(y != 0 ? TileBase::SmallRightEdge : TileBase::TopToRight);
                    else if (oside == tileHeight_ - 1)
                        tile = Tile(TileBase::RightEdge);
                    else
                        tile = Tile(TileBase::Empty);
                    continue;
                }
                tile = Tile(y == 0 ? TileBase::Top : TileBase::Normal);
            }
    }

    float pagodaSlopeAt(int yTile) const {
        const float fromBottom = float(tileHeight_ - yTile);
        const float heightIntersect = float(tileHeight_ - (overhang_ + 2)) * slope_;
        const float intersectY = float(tileHeight_ - 2);
        const float a = heightIntersect / (intersectY * intersectY);
        return 2 * a * fromBottom;
    }

    float maxHeight() const {
        if (pagoda_)
            return float(tileHeight_ - (overhang_ + 2)) * slope_ + pagodaSlopeAt(0) + pagodaSlopeAt(1);
        return float(tileHeight_ - overhang_) * slope_;
    }

    float pagodaCornerOffset(float x) const {
        const int corner = std::min(4, std::max(0, tileHeight_ - 4));
        if (x < float(corner) && lEnd_ == EdgeEnd::Normal) {
            const float s = float(corner) - x;
            return s * s * (1 / 16.0f) * slope_;
        }
        if (x > float(tileWidth_ - corner) && rEnd_ == EdgeEnd::Normal) {
            const float s = x - float(tileWidth_ - corner);
            return s * s * (1 / 16.0f) * slope_;
        }
        return 0;
    }

    // The roof's border: a trim strip on top, the edge below it, facing out.
    static void drawTrim(Out trim, Out edges, V3 from, V3 to) {
        static const std::array<std::array<float, 2>, 4> uv{{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
        const V3 d1{0, 0, -kThicknessTop}, d2{0, 0, -kThicknessInner};
        const std::array<V3, 4> a{from, to, to + d1, from + d1};
        const std::array<V3, 4> b{a[3], a[2], a[2] + d2, a[3] + d2};
        for (auto [out, q] : {std::pair{trim, a}, std::pair{edges, b}}) {
            const V3 n = faceNormal(q[0], q[2], q[1]);
            std::array<uint32_t, 4> k;
            for (size_t j = 0; j < 4; ++j)
                k[j] = out.add(q[j], uv[j], n);
            out.tri(k[0], k[2], k[1]);
            out.tri(k[2], k[0], k[3]);
        }
    }

    void cutUsing(const Tile& o, int x, int y, int dir) { at(x, y).cutWith(o.cut, dir); }

    bool isOverhang(int x, int y) const {
        return (x < overhang_ && lEnd_ != EdgeEnd::FlatShort) ||
               (x >= tileWidth_ - overhang_ && rEnd_ != EdgeEnd::FlatShort) || y >= tileHeight_ - overhang_ ||
               y < topOverhang_;
    }
    bool isOverhangX(int x, int) const {
        return (x < overhang_ && lEnd_ != EdgeEnd::FlatShort) ||
               (x >= tileWidth_ - overhang_ && rEnd_ != EdgeEnd::FlatShort);
    }
    int leftExtent() const { return lEnd_ == EdgeEnd::FlatShort ? 0 : overhang_; }
    int rightExtent() const { return rEnd_ == EdgeEnd::FlatShort ? 0 : overhang_; }
};

bool RoofEdge::intersect(RoofEdge& other) {
    const float heightOffset = height_ / slope_ - other.height_ / other.slope_;
    if (slope_ != other.slope_ || std::abs(heightOffset - std::nearbyint(heightOffset)) > kBias ||
        tileSize_ != other.tileSize_ || pagoda_ || other.pagoda_)
        return false;

    float dir = angleDeg(xNormal_, other.xNormal_);
    if (dot(xNormal_, other.yNormal_) < 0)
        dir = -dir;
    const float notches = dir / 90;
    if (std::abs(std::nearbyint(notches) - notches) > kBias)
        return false; // not at a multiple of 90 degrees
    const int dirDiff = (int(std::nearbyint(notches)) + 4) % 4;

    int xTO = roundToInt(dot(xNormal_, other.tl_ - tl_) / tileSize_);
    int yTO = roundToInt(dot(yNormal_, other.tl_ - tl_) / tileSize_);

    switch (dirDiff) {
    case 0: {
        // Parallel: where one face is above the other, remove the lower
        // tiles; at the same height keep the tile that shows both best.
        xTO += leftExtent() - other.leftExtent();
        const int heightOffsetTiles = yTO + (tileHeight_ - other.tileHeight_);
        const float heightDiff = (height_ - other.height_) + float(heightOffsetTiles) * slope_;
        const int xFrom = std::max(0, xTO), xTo = std::min(tileWidth_ - 1, xTO + other.tileWidth_ - 1);
        const int yFrom = std::max(0, -yTO), yTo = std::min(tileHeight_ - 1, (other.tileHeight_ - 1) - yTO);
        const int xFromOther = std::max(0, -xTO), yFromOther = std::max(0, yTO);
        if (std::abs(heightDiff) < kBias) {
            for (int y = yFrom, yO = yFromOther; y <= yTo; ++y, ++yO)
                for (int x = xFrom, xO = xFromOther; x <= xTo; ++x, ++xO) {
                    Tile& tile = at(x, y);
                    Tile& oTile = other.at(xO, yO);
                    if (tile.overlapPriority() > oTile.overlapPriority()) {
                        tile.addIntersectionsFrom(oTile);
                        oTile.erase();
                    } else {
                        oTile.addIntersectionsFrom(tile);
                        tile.erase();
                    }
                }
        } else {
            const bool thisHigher = heightDiff > 0;
            for (int y = yFrom, yO = yFromOther; y <= yTo; ++y, ++yO)
                for (int x = xFrom, xO = xFromOther; x <= xTo; ++x, ++xO) {
                    if (thisHigher) {
                        // Only real roof (not overhang) removes tiles on another level.
                        if (!isOverhang(x, y) || (height_ == other.height_ && other.isOverhangX(xO, yO)))
                            other.cutUsing(at(x, y), xO, yO, 0);
                    } else if (!other.isOverhang(xO, yO) || (height_ == other.height_ && isOverhangX(x, y))) {
                        cutUsing(other.at(xO, yO), x, y, 0);
                    }
                }
        }
        break;
    }
    case 1:
    case 3: {
        // At 90 degrees: the left face sees a / diagonal, the right one \.
        if (dirDiff == 3)
            std::tie(xTO, yTO) = std::pair{-yTO, xTO};
        RoofEdge& left = dirDiff == 3 ? other : *this;
        RoofEdge& right = dirDiff == 3 ? *this : other;
        xTO += left.leftExtent();
        yTO += right.leftExtent();
        const int xFrom = std::max(0, xTO - right.tileHeight_), xTo = std::min(left.tileWidth_ - 1, xTO - 1);
        const int yFrom = std::max(0, -yTO), yTo = std::min(left.tileHeight_ - 1, (right.tileWidth_ - 1) - yTO);
        const int xFromOther = std::max(0, yTO), yFromOther = std::min(right.tileHeight_ - 1, xTO - 1);
        const int heightNotches = roundToInt(left.height_ / left.slope_);
        const int oHeightNotches = roundToInt(right.height_ / right.slope_);
        for (int y = yFrom, xO = xFromOther; y <= yTo; ++y, ++xO)
            for (int x = xFrom, yO = yFromOther; x <= xTo; ++x, --yO) {
                const int tileHeight = heightNotches + (left.tileHeight_ - 1) - y;
                const int tileHeightOther = oHeightNotches + (right.tileHeight_ - 1) - yO;
                Tile& tile = left.at(x, y);
                Tile& oTile = right.at(xO, yO);
                if (tileHeight < tileHeightOther) {
                    if (!right.isOverhang(xO, yO))
                        left.cutUsing(oTile, x, y, 3);
                } else if (tileHeight == tileHeightOther) {
                    if (left.isOverhang(x, y) == right.isOverhang(xO, yO)) {
                        oTile.cutWith(uint8_t((tile.cut & CutTopLeft) | CutBottomRight), 1);
                        tile.cutWith(uint8_t((oTile.cut & CutTopRight) | CutBottomLeft), 3);
                    }
                    // Valley pictures on a face only if the other is not fully cut out.
                    if ((oTile.cut & CutTopRight) != CutTopRight) {
                        tile.addIntersection(TileIntersection::Left);
                        if (y > 0) {
                            left.at(x, y - 1).addIntersection(TileIntersection::SmallLeft);
                            if (x < left.tileWidth_ - 1)
                                left.at(x + 1, y - 1).addIntersection(TileIntersection::SmallRight);
                        }
                        if (x > 0 && y == yTo)
                            left.at(x - 1, y).addIntersection(TileIntersection::SmallLeft);
                    }
                    if ((tile.cut & CutTopLeft) != CutTopLeft) {
                        oTile.addIntersection(TileIntersection::Right);
                        if (yO > 0) {
                            right.at(xO, yO - 1).addIntersection(TileIntersection::SmallRight);
                            if (xO > 0)
                                right.at(xO - 1, yO - 1).addIntersection(TileIntersection::SmallLeft);
                        }
                        if (xO < right.tileWidth_ - 1)
                            right.at(xO + 1, yO).addIntersection(TileIntersection::SmallRight);
                    }
                } else if (!left.isOverhang(x, y)) {
                    right.cutUsing(tile, xO, yO, 1);
                }
            }
        break;
    }
    case 2: {
        // Opposite faces: remove the lower one where they cross and mark the ridge.
        xTO += leftExtent() + other.rightExtent();
        const int xFrom = std::max(0, xTO - other.tileWidth_), xTo = std::min(tileWidth_ - 1, xTO - 1);
        const int yFrom = std::max(0, -yTO - other.tileHeight_), yTo = std::min(tileHeight_ - 1, -yTO - 1);
        const int xFromOther = std::min(other.tileWidth_ - 1, xTO - 1);
        const int yFromOther = std::min(other.tileHeight_ - 1, -yTO - 1);
        const int heightNotches = roundToInt(height_ / slope_);
        const int oHeightNotches = roundToInt(other.height_ / other.slope_);
        for (int y = yFrom, yO = yFromOther; y <= yTo; ++y, --yO) {
            const int tileHeight = heightNotches + (tileHeight_ - 1) - y;
            const int tileHeightOther = oHeightNotches + (other.tileHeight_ - 1) - yO;
            for (int x = xFrom, xO = xFromOther; x <= xTo; ++x, --xO) {
                Tile& tile = at(x, y);
                Tile& oTile = other.at(xO, yO);
                if (tileHeight < tileHeightOther) {
                    if (!other.isOverhang(xO, yO) || (height_ == other.height_ && isOverhangX(x, y)))
                        cutUsing(oTile, x, y, dirDiff);
                    if (tileHeightOther == tileHeight + 1 && !(tile.ownBaseCut() & CutTop) && y > 0) {
                        if (xO > 0)
                            other.at(xO - 1, yO).addIntersection(TileIntersection::SmallLeft);
                        oTile.addIntersection(TileIntersection::Bottom);
                        if (xO < other.tileWidth_ - 1)
                            other.at(xO + 1, yO).addIntersection(TileIntersection::SmallRight);
                    }
                } else {
                    if (!isOverhang(x, y) || (height_ == other.height_ && other.isOverhangX(xO, yO)))
                        other.cutUsing(tile, xO, yO, dirDiff);
                    if (tileHeight == tileHeightOther + 1 && !(oTile.ownBaseCut() & CutTop) && yO > 0) {
                        if (x > 0)
                            at(x - 1, y).addIntersection(TileIntersection::SmallLeft);
                        tile.addIntersection(TileIntersection::Bottom);
                        if (x < tileWidth_ - 1)
                            at(x + 1, y).addIntersection(TileIntersection::SmallRight);
                    }
                }
            }
        }
        break;
    }
    }
    return true;
}

// --- Roof types (AbstractSimpleRoof and subclasses) ---

struct LotRoofs::Roof {
    int level = 0;
    std::vector<RoofEdge> edges;

    float heightAt(float x, float y) const {
        for (const auto& e : edges)
            if (float h = e.heightAt(x, y); h != kInf)
                return h;
        return kInf;
    }

    void intersect(Roof& other) {
        for (auto& e : edges)
            for (auto& o : other.edges)
                if (!e.intersect(o)) {
                    e.removeTilesUnder([&](float x, float y) { return other.heightAt(x, y); });
                    o.removeTilesUnder([&](float x, float y) { return heightAt(x, y); });
                }
    }
};

namespace {

// Direction a roof's drag vector points to: 0 = +X, 1 = +Y, 2 = -X, 3 = -Y.
// (As in OpenTS2, the comparison picks the axis that is not the larger one.)
int determineDirection(V2 v) {
    if (std::abs(v.y) > std::abs(v.x))
        return v.x > 0 ? 0 : 2;
    return v.y > 0 ? 1 : 3;
}
V2 rotate(V2 v, int dir) {
    switch (dir) {
    case 1: return {-v.y, v.x};
    case 2: return {-v.x, -v.y};
    case 3: return {v.y, -v.x};
    default: return v;
    }
}
// Diagonal roofs: the drag vector as components along (1, 1) and (-1, 1).
V2 diagonalVector(const Lot::Roof& r) {
    const V2 v{r.x1 - r.x0, r.y1 - r.y0};
    const float yD = (v.y - v.x) / 2;
    return {v.y - yD, yD};
}
V2 fromDiagonal(V2 d) { return {d.x - d.y, d.x + d.y}; }

struct Corners {
    V2 bl, br, tr, tl;
};
Corners straightCorners(const Lot::Roof& r) {
    const V2 bl{std::min(r.x0, r.x1), std::min(r.y0, r.y1)}, tr{std::max(r.x0, r.x1), std::max(r.y0, r.y1)};
    return {bl, {tr.x, bl.y}, tr, {bl.x, tr.y}};
}
// Diagonal corners, and the extents along the two diagonal axes.
Corners diagonalCorners(const Lot::Roof& r, V2& lo, V2& hi) {
    const V2 d = diagonalVector(r), from{r.x0, r.y0};
    lo = {std::min(0.0f, d.x), std::min(0.0f, d.y)};
    hi = {std::max(0.0f, d.x), std::max(0.0f, d.y)};
    return {from + fromDiagonal(lo), from + fromDiagonal({hi.x, lo.y}), from + fromDiagonal(hi),
            from + fromDiagonal({lo.x, hi.y})};
}

std::vector<RoofEdge> gable(const Lot::Roof& r, float h, bool isLong, bool pagoda, bool diag) {
    V2 lo, hi;
    const Corners c = diag ? diagonalCorners(r, lo, hi) : straightCorners(r);
    const int dir = diag ? determineDirection(diagonalVector(r)) : determineDirection({r.x1 - r.x0, r.y1 - r.y0});
    const bool flatInX = (dir % 2) == (isLong ? 1 : 0);
    const float s = r.angle;
    if (flatInX) {
        const V2 toTop = diag ? fromDiagonal({0, (hi.y - lo.y) / 2}) : V2{0, (c.tr.y - c.br.y) / 2};
        return {RoofEdge(h, s, c.bl, c.br, c.br + toTop, c.bl + toTop, EdgeEnd::Flat, EdgeEnd::Flat, 0, pagoda),
                RoofEdge(h, s, c.tr, c.tl, c.tl - toTop, c.tr - toTop, EdgeEnd::Flat, EdgeEnd::Flat, 0, pagoda)};
    }
    const V2 toTop = diag ? fromDiagonal({(hi.x - lo.x) / 2, 0}) : V2{(c.tr.x - c.tl.x) / 2, 0};
    return {RoofEdge(h, s, c.br, c.tr, c.tr - toTop, c.br - toTop, EdgeEnd::Flat, EdgeEnd::Flat, 0, pagoda),
            RoofEdge(h, s, c.tl, c.bl, c.bl + toTop, c.tl + toTop, EdgeEnd::Flat, EdgeEnd::Flat, 0, pagoda)};
}

std::vector<RoofEdge> hip(const Lot::Roof& r, float h, bool pagoda, bool diag, bool mansard) {
    V2 lo, hi;
    const Corners c = diag ? diagonalCorners(r, lo, hi) : straightCorners(r);
    V2 minX, minY;
    if (diag) {
        const V2 d = diagonalVector(r);
        const float m = std::min(std::abs(d.x), std::abs(d.y)) / 2;
        minX = fromDiagonal({m, 0}), minY = fromDiagonal({0, m});
    } else {
        const V2 size = c.tr - c.bl;
        float m = std::min(size.x, size.y) / 2;
        if (mansard)
            m = std::min(3.0f, m); // OpenTS2: flat top beyond 3 tiles in
        minX = {m, 0}, minY = {0, m};
    }
    const float s = r.angle;
    return {RoofEdge(h, s, c.bl, c.br, c.br + minY, c.bl + minY, EdgeEnd::Normal, EdgeEnd::Normal, 0, pagoda),
            RoofEdge(h, s, c.br, c.tr, c.tr - minX, c.br - minX, EdgeEnd::Normal, EdgeEnd::Normal, 0, pagoda),
            RoofEdge(h, s, c.tr, c.tl, c.tl - minY, c.tr - minY, EdgeEnd::Normal, EdgeEnd::Normal, 0, pagoda),
            RoofEdge(h, s, c.tl, c.bl, c.bl + minX, c.tl + minX, EdgeEnd::Normal, EdgeEnd::Normal, 0, pagoda)};
}

std::vector<RoofEdge> shedGable(const Lot::Roof& r, float h, bool pagoda, bool diag) {
    V2 lo, hi;
    const Corners c = diag ? diagonalCorners(r, lo, hi) : straightCorners(r);
    const int dir = diag ? determineDirection(diagonalVector(r)) : determineDirection({r.x1 - r.x0, r.y1 - r.y0});
    // Diagonal shed roofs have an overhang at the top too.
    const V2 over = diag && !pagoda ? fromDiagonal(rotate({0.5f, 0}, dir)) : V2{};
    const int top = diag ? 2 : 0;
    const float s = r.angle;
    auto edge = [&](V2 a, V2 b, V2 cc, V2 d) {
        return std::vector<RoofEdge>{RoofEdge(h, s, a, b, cc + over, d + over, EdgeEnd::Flat, EdgeEnd::Flat, top, pagoda)};
    };
    switch (dir) {
    case 0: return edge(c.tl, c.bl, c.br, c.tr); // wall edge towards +X
    case 1: return edge(c.bl, c.br, c.tr, c.tl);
    case 2: return edge(c.br, c.tr, c.tl, c.bl);
    default: return edge(c.tr, c.tl, c.bl, c.br);
    }
}

std::vector<RoofEdge> shedHip(const Lot::Roof& r, float h, bool diag) {
    V2 lo, hi;
    const Corners c = diag ? diagonalCorners(r, lo, hi) : straightCorners(r);
    const int dir = diag ? determineDirection(diagonalVector(r)) : determineDirection({r.x1 - r.x0, r.y1 - r.y0});
    const std::array<V2, 4> corners{c.tl, c.bl, c.br, c.tr};
    const bool flatInX = dir % 2 == 0;
    const float w = diag ? hi.x - lo.x : c.tr.x - c.bl.x, d = diag ? hi.y - lo.y : c.tr.y - c.bl.y;
    const float minDim = flatInX ? std::min(w, d / 2) : std::min(w / 2, d);
    V2 towards = rotate({minDim, 0}, dir), towardsTop = rotate({0, minDim}, dir);
    if (diag) {
        towards = fromDiagonal(towards) + fromDiagonal(rotate({0.5f, 0}, dir));
        towardsTop = fromDiagonal(towardsTop);
    }
    const V2 c1 = corners[size_t(dir)], c2 = corners[size_t((dir + 1) % 4)], c3 = corners[size_t((dir + 2) % 4)],
             c4 = corners[size_t((dir + 3) % 4)];
    const EdgeEnd flat = diag ? EdgeEnd::Flat : EdgeEnd::FlatShort;
    const float s = r.angle;
    return {RoofEdge(h, s, c4, c1, c1 - towardsTop, c4 - towardsTop, flat, EdgeEnd::Normal),
            RoofEdge(h, s, c1, c2, c2 + towards, c1 + towards, EdgeEnd::Normal, EdgeEnd::Normal, diag ? 2 : 0),
            RoofEdge(h, s, c2, c3, c3 + towardsTop, c2 + towardsTop, EdgeEnd::Normal, flat)};
}

} // namespace

LotRoofs::LotRoofs(const Lot& lot) {
    for (const auto& r : lot.roofs) {
        pattern_ = r.pattern;
        const float h = lot.elevationAt(r.level, std::clamp(r.x0, 0.0f, float(lot.width)),
                                        std::clamp(r.y0, 0.0f, float(lot.height)));
        auto roof = std::make_unique<Roof>();
        roof->level = r.level;
        switch (r.type) {
        case Lot::ShedGable: roof->edges = shedGable(r, h, false, false); break;
        case Lot::ShedHip: roof->edges = shedHip(r, h, false); break;
        case Lot::ShortGable: roof->edges = gable(r, h, false, false, false); break;
        case Lot::LongGable: roof->edges = gable(r, h, true, false, false); break;
        case Lot::Hip: roof->edges = hip(r, h, false, false, false); break;
        case Lot::Mansard: roof->edges = hip(r, h, false, false, true); break;
        case Lot::DiagonalLongGable: roof->edges = gable(r, h, true, false, true); break;
        case Lot::DiagonalShortGable: roof->edges = gable(r, h, false, false, true); break;
        case Lot::DiagonalHip: roof->edges = hip(r, h, false, true, false); break;
        case Lot::DiagonalShedGable: roof->edges = shedGable(r, h, false, true); break;
        case Lot::DiagonalShedHip: roof->edges = shedHip(r, h, true); break;
        case Lot::PagodaHip: roof->edges = hip(r, h, true, false, false); break;
        case Lot::PagodaLongGable: roof->edges = gable(r, h, true, true, false); break;
        case Lot::PagodaShedGable: roof->edges = shedGable(r, h, true, false); break;
        case Lot::DiagonalPagodaHip: roof->edges = hip(r, h, true, true, false); break;
        case Lot::DiagonalPagodaLongGable: roof->edges = gable(r, h, true, true, true); break;
        case Lot::DiagonalPagodaShedGable: roof->edges = shedGable(r, h, true, true); break;
        default:
            warnings.push_back("roof " + std::to_string(r.id) + ": type " + std::to_string(r.type) + " not supported");
            continue;
        }
        for (auto& existing : roofs_)
            existing->intersect(*roof);
        roofs_.push_back(std::move(roof));
    }
    if (pattern_ == 0)
        pattern_ = kDefaultPattern;
}

LotRoofs::~LotRoofs() = default;
LotRoofs::LotRoofs(LotRoofs&&) noexcept = default;
LotRoofs& LotRoofs::operator=(LotRoofs&&) noexcept = default;

LotRoofs::Meshes LotRoofs::build() const {
    Meshes m;
    for (const auto& r : roofs_)
        for (const auto& e : r->edges)
            e.generate(Out{&m.top}, Out{&m.edges}, Out{&m.trim}, Out{&m.under});
    return m;
}

float LotRoofs::heightAt(float x, float y, int level, float fallback, float offset) const {
    float best = -kInf;
    for (const auto& r : roofs_)
        if (r->level == level)
            if (float h = r->heightAt(x, y); h != kInf && h > best)
                best = h;
    return best == -kInf ? fallback : best - offset;
}

} // namespace os2
