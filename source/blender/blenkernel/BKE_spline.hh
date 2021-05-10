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

#pragma once

/** \file
 * \ingroup bke
 */

#include <mutex>

#include "FN_generic_virtual_array.hh"

#include "BLI_float3.hh"
#include "BLI_float4x4.hh"
#include "BLI_vector.hh"

#include "BKE_attribute_math.hh"

struct Curve;

class Spline;
using SplinePtr = std::unique_ptr<Spline>;

/**
 * A spline is an abstraction of a single branch-less curve section, its evaluation methods,
 * and data. The spline data itself is just control points and a set of attributes by the set
 * of "evaluated" data is often used instead.
 *
 * Any derived class of Spline has to manage two things:
 *  1. Interpolating arbitrary attribute data from the control points to evaluated points.
 *  2. Evaluating the positions based on the stored control point data.
 *
 * Beyond that, everything is the base class's responsibility, with minor exceptions. Further
 * evaluation happens in a layer on top of the evaluated points generated by the derived types.
 *
 * There are a few methods to evaluate a spline:
 *  1. #evaluated_positions and #interpolate_to_evaluated_points give data at the initial
 *     evaluated points, depending on the resolution.
 *  2. #lookup_evaluated_factor and #lookup_evaluated_factor are meant for one-off lookups
 *     along the length of a curve.
 *
 * Commonly used evaluated data is stored in caches on the spline itself so that operations on
 * splines don't need to worry about taking ownership of evaluated data when they don't need to.
 */
class Spline {
 public:
  enum class Type {
    Bezier,
    NURBS,
    Poly,
  };

 protected:
  Type type_;
  bool is_cyclic_ = false;

 public:
  enum NormalCalculationMode {
    ZUp,
    Minimum,
    Tangent,
  };
  /* Only #Zup is supported at the moment. */
  NormalCalculationMode normal_mode;

 protected:
  /** Direction of the spline at each evaluated point. */
  mutable blender::Vector<blender::float3> evaluated_tangents_cache_;
  mutable std::mutex tangent_cache_mutex_;
  mutable bool tangent_cache_dirty_ = true;

  /** Normal direction vectors for each evaluated point. */
  mutable blender::Vector<blender::float3> evaluated_normals_cache_;
  mutable std::mutex normal_cache_mutex_;
  mutable bool normal_cache_dirty_ = true;

  /** Accumulated lengths along the evaluated points. */
  mutable blender::Vector<float> evaluated_lengths_cache_;
  mutable std::mutex length_cache_mutex_;
  mutable bool length_cache_dirty_ = true;

 public:
  virtual ~Spline() = default;
  Spline(const Type type) : type_(type)
  {
  }
  Spline(Spline &other)
      : type_(other.type_), is_cyclic_(other.is_cyclic_), normal_mode(other.normal_mode)
  {
  }

  virtual SplinePtr copy() const = 0;

  Spline::Type type() const;

  /** Return the number of control points. */
  virtual int size() const = 0;
  int segments_size() const;
  bool is_cyclic() const;
  void set_cyclic(const bool value);

  virtual void resize(const int size) = 0;
  virtual blender::MutableSpan<blender::float3> positions() = 0;
  virtual blender::Span<blender::float3> positions() const = 0;
  virtual blender::MutableSpan<float> radii() = 0;
  virtual blender::Span<float> radii() const = 0;
  virtual blender::MutableSpan<float> tilts() = 0;
  virtual blender::Span<float> tilts() const = 0;

  virtual void translate(const blender::float3 &translation);
  virtual void transform(const blender::float4x4 &matrix);

  /**
   * Mark all caches for re-computation. This must be called after any operation that would
   * change the generated positions, tangents, normals, mapping, etc. of the evaluated points.
   */
  virtual void mark_cache_invalid() = 0;
  virtual int evaluated_points_size() const = 0;
  int evaluated_edges_size() const;

  float length() const;

  virtual blender::Span<blender::float3> evaluated_positions() const = 0;

  blender::Span<float> evaluated_lengths() const;
  blender::Span<blender::float3> evaluated_tangents() const;
  blender::Span<blender::float3> evaluated_normals() const;

  void bounds_min_max(blender::float3 &min, blender::float3 &max, const bool use_evaluated) const;

  struct LookupResult {
    /**
     * The index of the evaluated point before the result location. In other words, the index of
     * the edge that the result lies on. If the sampled factor/length is the very end of the
     * spline, this will be the second to last index, if it's the very beginning, this will be 0.
     */
    int evaluated_index;
    /**
     * The index of the evaluated point after the result location, accounting for wrapping when
     * the spline is cyclic. If the sampled factor/length is the very end of the spline, this will
     * be the last index (#evaluated_points_size - 1).
     */
    int next_evaluated_index;
    /**
     * The portion of the way from the evaluated point at #evaluated_index to the next point.
     * If the sampled factor/length is the very end of the spline, this will be the 1.0f
     */
    float factor;
  };
  LookupResult lookup_evaluated_factor(const float factor) const;
  LookupResult lookup_evaluated_length(const float length) const;

  blender::Array<float> sample_uniform_index_factors(const int samples_size) const;
  LookupResult lookup_data_from_index_factor(const float index_factor) const;

  /**
   * Interpolate a virtual array of data with the size of the number of control points to the
   * evaluated points. For poly splines, the lifetime of the returned virtual array must not
   * exceed the lifetime of the input data.
   */
  virtual blender::fn::GVArrayPtr interpolate_to_evaluated_points(
      const blender::fn::GVArray &source_data) const = 0;

 protected:
  virtual void correct_end_tangents() const = 0;
};

/**
 * A Bézier spline is made up of a many curve segments, possibly achieving continuity of curvature
 * by constraining the alignment of curve handles. Evaluation stores the positions and a map of
 * factors and indices in a list of floats, which is then used to interpolate any other data.
 */
class BezierSpline final : public Spline {
 public:
  enum class HandleType {
    /** The handle can be moved anywhere, and doesn't influence the point's other handle. */
    Free,
    /** The location is automatically calculated to be smooth. */
    Auto,
    /** The location is calculated to point to the next/previous control point. */
    Vector,
    /** The location is constrained to point in the opposite direction as the other handle. */
    Align,
  };

 private:
  blender::Vector<HandleType> handle_types_left_;
  blender::Vector<blender::float3> handle_positions_left_;
  blender::Vector<blender::float3> positions_;
  blender::Vector<HandleType> handle_types_right_;
  blender::Vector<blender::float3> handle_positions_right_;
  blender::Vector<float> radii_;
  blender::Vector<float> tilts_;
  int resolution_;

  /** Start index in evaluated points array for every control point. */
  mutable blender::Vector<int> offset_cache_;
  mutable std::mutex offset_cache_mutex_;
  mutable bool offset_cache_dirty_ = true;

  /** Cache of evaluated positions. */
  mutable blender::Vector<blender::float3> evaluated_position_cache_;
  mutable std::mutex position_cache_mutex_;
  mutable bool position_cache_dirty_ = true;

  /** Cache of "index factors" based calculated from the evaluated positions. */
  mutable blender::Vector<float> evaluated_mapping_cache_;
  mutable std::mutex mapping_cache_mutex_;
  mutable bool mapping_cache_dirty_ = true;

 public:
  virtual SplinePtr copy() const final;
  BezierSpline() : Spline(Type::Bezier)
  {
  }
  BezierSpline(const BezierSpline &other)
      : Spline((Spline &)other),
        handle_types_left_(other.handle_types_left_),
        handle_positions_left_(other.handle_positions_left_),
        positions_(other.positions_),
        handle_types_right_(other.handle_types_right_),
        handle_positions_right_(other.handle_positions_right_),
        radii_(other.radii_),
        tilts_(other.tilts_),
        resolution_(other.resolution_)
  {
  }

  int size() const final;
  int resolution() const;
  void set_resolution(const int value);

  void add_point(const blender::float3 position,
                 const HandleType handle_type_start,
                 const blender::float3 handle_position_start,
                 const HandleType handle_type_end,
                 const blender::float3 handle_position_end,
                 const float radius,
                 const float tilt);

  void resize(const int size) final;
  blender::MutableSpan<blender::float3> positions() final;
  blender::Span<blender::float3> positions() const final;
  blender::MutableSpan<float> radii() final;
  blender::Span<float> radii() const final;
  blender::MutableSpan<float> tilts() final;
  blender::Span<float> tilts() const final;
  blender::Span<HandleType> handle_types_left() const;
  blender::MutableSpan<HandleType> handle_types_left();
  blender::Span<blender::float3> handle_positions_left() const;
  blender::MutableSpan<blender::float3> handle_positions_left();
  blender::Span<HandleType> handle_types_right() const;
  blender::MutableSpan<HandleType> handle_types_right();
  blender::Span<blender::float3> handle_positions_right() const;
  blender::MutableSpan<blender::float3> handle_positions_right();

  void translate(const blender::float3 &translation) override;
  void transform(const blender::float4x4 &matrix) override;

  bool point_is_sharp(const int index) const;

  void mark_cache_invalid() final;
  int evaluated_points_size() const final;

  blender::Span<int> control_point_offsets() const;
  blender::Span<float> evaluated_mappings() const;
  blender::Span<blender::float3> evaluated_positions() const final;
  struct InterpolationData {
    int control_point_index;
    int next_control_point_index;
    /**
     * Linear interpolation weight between the two indices, from 0 to 1.
     * Higher means next control point.
     */
    float factor;
  };
  InterpolationData interpolation_data_from_index_factor(const float index_factor) const;

  virtual blender::fn::GVArrayPtr interpolate_to_evaluated_points(
      const blender::fn::GVArray &source_data) const;

 private:
  void correct_end_tangents() const final;
  bool segment_is_vector(const int start_index) const;
  void evaluate_bezier_segment(const int index,
                               const int next_index,
                               blender::MutableSpan<blender::float3> positions) const;
  blender::Array<int> evaluated_point_offsets() const;
};

/**
 * Data for Non-Uniform Rational B-Splines. The mapping from control points to evaluated points is
 * influenced by a vector of knots, weights for each point, and the order of the spline. Every
 * mapping of data to evaluated points is handled the same way, but the positions are cached in
 * the spline.
 */
class NURBSpline final : public Spline {
 public:
  enum class KnotsMode {
    Normal,
    EndPoint,
    Bezier,
  };
  KnotsMode knots_mode;

  struct BasisCache {
    /** The influence at each control point `i + #start_index`. */
    blender::Vector<float> weights;
    /**
     * An offset for the start of #weights: the first control point index with a non-zero weight.
     */
    int start_index;
  };

 private:
  blender::Vector<blender::float3> positions_;
  blender::Vector<float> radii_;
  blender::Vector<float> tilts_;
  blender::Vector<float> weights_;
  int resolution_;
  /**
   * Defines the number of nearby control points that influence a given evaluated point. Higher
   * orders give smoother results. The number of control points must be greater than or equal to
   * this value.
   */
  uint8_t order_;

  /**
   * Determines where and how the control points affect the evaluated points. The length should
   * always be the value returned by #knots_size(), and each value should be greater than or equal
   * to the previous. Only invalidated when a point is added or removed.
   */
  mutable blender::Vector<float> knots_;
  mutable std::mutex knots_mutex_;
  mutable bool knots_dirty_ = true;

  /** Cache of control point influences on each evaluated point. */
  mutable blender::Vector<BasisCache> basis_cache_;
  mutable std::mutex basis_cache_mutex_;
  mutable bool basis_cache_dirty_ = true;

  /**
   * Cache of position data calculated from the basis cache. Though it is interpolated
   * in the same way as any other attribute, it is stored to save unnecessary recalculation.
   */
  mutable blender::Vector<blender::float3> evaluated_position_cache_;
  mutable std::mutex position_cache_mutex_;
  mutable bool position_cache_dirty_ = true;

 public:
  SplinePtr copy() const final;
  NURBSpline() : Spline(Type::NURBS)
  {
  }
  NURBSpline(const NURBSpline &other)
      : Spline((Spline &)other),
        knots_mode(other.knots_mode),
        positions_(other.positions_),
        radii_(other.radii_),
        tilts_(other.tilts_),
        weights_(other.weights_),
        resolution_(other.resolution_),
        order_(other.order_)
  {
  }

  int size() const final;
  int resolution() const;
  void set_resolution(const int value);
  uint8_t order() const;
  void set_order(const uint8_t value);

  void add_point(const blender::float3 position,
                 const float radius,
                 const float tilt,
                 const float weight);

  bool check_valid_size_and_order() const;
  int knots_size() const;

  void resize(const int size) final;
  blender::MutableSpan<blender::float3> positions() final;
  blender::Span<blender::float3> positions() const final;
  blender::MutableSpan<float> radii() final;
  blender::Span<float> radii() const final;
  blender::MutableSpan<float> tilts() final;
  blender::Span<float> tilts() const final;
  blender::Span<float> knots() const;

  blender::MutableSpan<float> weights();
  blender::Span<float> weights() const;

  void mark_cache_invalid() final;
  int evaluated_points_size() const final;

  blender::Span<blender::float3> evaluated_positions() const final;

  blender::fn::GVArrayPtr interpolate_to_evaluated_points(
      const blender::fn::GVArray &source_data) const final;

 protected:
  void correct_end_tangents() const final;
  void calculate_knots() const;
  void calculate_basis_cache() const;
};

/**
 * A Poly spline is like a bezier spline with a resolution of one. The main reason to distinguish
 * the two is for reduced complexity and increased performance, since interpolating data to control
 * points does not change it.
 */
class PolySpline final : public Spline {
 public:
  blender::Vector<blender::float3> positions_;
  blender::Vector<float> radii_;
  blender::Vector<float> tilts_;

 private:
 public:
  SplinePtr copy() const final;
  PolySpline() : Spline(Type::Poly)
  {
  }
  PolySpline(const PolySpline &other)
      : Spline((Spline &)other),
        positions_(other.positions_),
        radii_(other.radii_),
        tilts_(other.tilts_)
  {
  }

  int size() const final;

  void add_point(const blender::float3 position, const float radius, const float tilt);

  void resize(const int size) final;
  blender::MutableSpan<blender::float3> positions() final;
  blender::Span<blender::float3> positions() const final;
  blender::MutableSpan<float> radii() final;
  blender::Span<float> radii() const final;
  blender::MutableSpan<float> tilts() final;
  blender::Span<float> tilts() const final;

  void mark_cache_invalid() final;
  int evaluated_points_size() const final;

  blender::Span<blender::float3> evaluated_positions() const final;

  blender::fn::GVArrayPtr interpolate_to_evaluated_points(
      const blender::fn::GVArray &source_data) const final;

 protected:
  void correct_end_tangents() const final;
};

/**
 * A #CurveEval corresponds to the #Curve object data. The name is different for clarity, since
 * more of the data is stored in the splines, but also just to be different than the name in DNA.
 */
class CurveEval {
 public:
  blender::Vector<SplinePtr> splines;

  CurveEval *copy();

  void translate(const blender::float3 &translation);
  void transform(const blender::float4x4 &matrix);
  void bounds_min_max(blender::float3 &min, blender::float3 &max, const bool use_evaluated) const;
};

std::unique_ptr<CurveEval> curve_eval_from_dna_curve(const Curve &curve);