/**************************************************************************/
/*  jolt_area_3d.cpp                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "jolt_area_3d.h"

#include "../jolt_project_settings.h"
#include "../misc/jolt_math_funcs.h"
#include "../misc/jolt_type_conversions.h"
#include "../shapes/jolt_shape_3d.h"
#include "../spaces/jolt_broad_phase_layer.h"
#include "../spaces/jolt_space_3d.h"
#include "jolt_body_3d.h"
#include "jolt_group_filter.h"
#include "jolt_soft_body_3d.h"

namespace {

constexpr double AREA_DEFAULT_WIND_MAGNITUDE = 0.0;
constexpr double AREA_DEFAULT_WIND_ATTENUATION = 0.0;

const Vector3 AREA_DEFAULT_WIND_SOURCE = Vector3();
const Vector3 AREA_DEFAULT_WIND_DIRECTION = Vector3();

} // namespace

JPH::BroadPhaseLayer JoltArea3D::_get_broad_phase_layer() const {
	return monitorable ? JoltBroadPhaseLayer::AREA_DETECTABLE : JoltBroadPhaseLayer::AREA_UNDETECTABLE;
}

JPH::ObjectLayer JoltArea3D::_get_object_layer() const {
	ERR_FAIL_NULL_V(space, 0);

	if (jolt_shape == nullptr || jolt_shape->GetType() == JPH::EShapeType::Empty) {
		// No point doing collision checks against a shapeless object.
		return space->map_to_object_layer(_get_broad_phase_layer(), 0, 0);
	}

	return space->map_to_object_layer(_get_broad_phase_layer(), collision_layer, collision_mask);
}

void JoltArea3D::_add_to_space() {
	jolt_shape = build_shapes(true);

	JPH::CollisionGroup::GroupID group_id = 0;
	JPH::CollisionGroup::SubGroupID sub_group_id = 0;
	JoltGroupFilter::encode_object(this, group_id, sub_group_id);

	jolt_settings->mUserData = reinterpret_cast<JPH::uint64>(this);
	jolt_settings->mObjectLayer = _get_object_layer();
	jolt_settings->mCollisionGroup = JPH::CollisionGroup(nullptr, group_id, sub_group_id);
	jolt_settings->mMotionType = _get_motion_type();
	jolt_settings->mIsSensor = true;
	jolt_settings->mCollideKinematicVsNonDynamic = true;
	jolt_settings->mUseManifoldReduction = false;
	jolt_settings->mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
	jolt_settings->mMassPropertiesOverride.mMass = 1.0f;
	jolt_settings->mMassPropertiesOverride.mInertia = JPH::Mat44::sIdentity();

	jolt_settings->SetShape(jolt_shape);

	JPH::Body *new_jolt_body = space->add_object(*this, *jolt_settings, _should_sleep());
	if (new_jolt_body == nullptr) {
		return;
	}

	jolt_body = new_jolt_body;

	delete jolt_settings;
	jolt_settings = nullptr;
}

void JoltArea3D::_enqueue_call_queries() {
	if (space != nullptr) {
		space->enqueue_call_queries(&call_queries_element);
	}
}

void JoltArea3D::_dequeue_call_queries() {
	if (space != nullptr) {
		space->dequeue_call_queries(&call_queries_element);
	}
}

void JoltArea3D::_add_shape_pair(Overlap &p_overlap, const JPH::BodyID &p_body_id, const JPH::SubShapeID &p_other_shape_id, const JPH::SubShapeID &p_self_shape_id) {
	const JoltShapedObject3D *other_object = space->try_get_shaped(p_body_id);
	ERR_FAIL_NULL(other_object);

	p_overlap.rid = other_object->get_rid();
	p_overlap.instance_id = other_object->get_instance_id();

	HashMap<ShapeIDPair, ShapeIndexPair, ShapeIDPair>::Iterator shape_pair = p_overlap.shape_pairs.find(ShapeIDPair(p_other_shape_id, p_self_shape_id));
	if (shape_pair == p_overlap.shape_pairs.end()) {
		const int other_shape_index = other_object->find_shape_index(p_other_shape_id);
		const int self_shape_index = find_shape_index(p_self_shape_id);
		shape_pair = p_overlap.shape_pairs.insert(ShapeIDPair(p_other_shape_id, p_self_shape_id), ShapeIndexPair(other_shape_index, self_shape_index));
	}

	p_overlap.pending_added.push_back(shape_pair->value);

	_events_changed();
}

bool JoltArea3D::_remove_shape_pair(Overlap &p_overlap, const JPH::SubShapeID &p_other_shape_id, const JPH::SubShapeID &p_self_shape_id) {
	HashMap<ShapeIDPair, ShapeIndexPair, ShapeIDPair>::Iterator shape_pair = p_overlap.shape_pairs.find(ShapeIDPair(p_other_shape_id, p_self_shape_id));

	if (shape_pair == p_overlap.shape_pairs.end()) {
		return false;
	}

	p_overlap.pending_removed.push_back(shape_pair->value);
	p_overlap.shape_pairs.remove(shape_pair);

	_events_changed();

	return true;
}

void JoltArea3D::_flush_events(OverlapsById &p_objects, const Callable &p_callback) {
	for (OverlapsById::Iterator E = p_objects.begin(); E;) {
		Overlap &overlap = E->value;

		if (p_callback.is_valid()) {
			for (const ShapeIndexPair &shape_indices : overlap.pending_added) {
				int &ref_count = overlap.ref_counts[shape_indices];
				if (ref_count++ == 0) {
					_report_event(p_callback, PhysicsServer3D::AREA_BODY_ADDED, overlap.rid, overlap.instance_id, shape_indices.other, shape_indices.self);
				}
			}

			for (const ShapeIndexPair &shape_indices : overlap.pending_removed) {
				int &ref_count = overlap.ref_counts[shape_indices];
				ERR_CONTINUE(ref_count <= 0);
				if (--ref_count == 0) {
					_report_event(p_callback, PhysicsServer3D::AREA_BODY_REMOVED, overlap.rid, overlap.instance_id, shape_indices.other, shape_indices.self);
					overlap.ref_counts.erase(shape_indices);
				}
			}
		}

		overlap.pending_removed.clear();
		overlap.pending_added.clear();

		OverlapsById::Iterator next = E;
		++next;

		if (overlap.shape_pairs.is_empty()) {
			p_objects.remove(E);
		}

		E = next;
	}
}

void JoltArea3D::_report_event(const Callable &p_callback, PhysicsServer3D::AreaBodyStatus p_status, const RID &p_other_rid, ObjectID p_other_instance_id, int p_other_shape_index, int p_self_shape_index) const {
	ERR_FAIL_COND(!p_callback.is_valid());

	const Variant arg1 = p_status;
	const Variant arg2 = p_other_rid;
	const Variant arg3 = p_other_instance_id;
	const Variant arg4 = p_other_shape_index;
	const Variant arg5 = p_self_shape_index;
	const Variant *args[5] = { &arg1, &arg2, &arg3, &arg4, &arg5 };

	Callable::CallError ce;
	Variant ret;
	p_callback.callp(args, 5, ret, ce);

	if (unlikely(ce.error != Callable::CallError::CALL_OK)) {
		ERR_PRINT_ONCE(vformat("Failed to call area monitor callback for '%s'. It returned the following error: '%s'.", to_string(), Variant::get_callable_error_text(p_callback, args, 5, ce)));
	}
}

void JoltArea3D::_notify_body_entered(const JPH::BodyID &p_body_id) {
	if (JoltBody3D *other_body = space->try_get_body(p_body_id)) {
		other_body->add_area(this);
	}
}

void JoltArea3D::_notify_body_exited(const JPH::BodyID &p_body_id) {
	if (JoltBody3D *other_body = space->try_get_body(p_body_id)) {
		other_body->remove_area(this);
	}
}

void JoltArea3D::_remove_all_overlaps() {
	for (KeyValue<JPH::BodyID, Overlap> &E : bodies_by_id) {
		_notify_body_exited(E.key);
	}

	bodies_by_id.clear();
	areas_by_id.clear();
}

void JoltArea3D::_update_sleeping() {
	if (!in_space()) {
		return;
	}

	space->set_is_object_sleeping(jolt_body->GetID(), _should_sleep());
}

void JoltArea3D::_update_group_filter() {
	if (!in_space()) {
		return;
	}

	jolt_body->GetCollisionGroup().SetGroupFilter(JoltGroupFilter::instance);
}

void JoltArea3D::_update_default_gravity() {
	if (is_default_area()) {
		space->get_physics_system().SetGravity(to_jolt(gravity_vector) * gravity);
	}
}

void JoltArea3D::_space_changing() {
	JoltShapedObject3D::_space_changing();

	_remove_all_overlaps();
	_dequeue_call_queries();
}

void JoltArea3D::_space_changed() {
	JoltShapedObject3D::_space_changed();

	_update_group_filter();
	_update_default_gravity();
}

void JoltArea3D::_events_changed() {
	_enqueue_call_queries();
}

void JoltArea3D::_body_monitoring_changed() {
	_update_sleeping();
}

void JoltArea3D::_area_monitoring_changed() {
	_update_sleeping();
}

void JoltArea3D::_monitorable_changed() {
	_update_object_layer();
}

void JoltArea3D::_gravity_changed() {
	_update_default_gravity();
}

JoltArea3D::JoltArea3D() :
		JoltShapedObject3D(OBJECT_TYPE_AREA),
		call_queries_element(this) {
}

bool JoltArea3D::is_default_area() const {
	return space != nullptr && space->get_default_area() == this;
}

void JoltArea3D::set_default_area(bool p_value) {
	if (p_value) {
		_update_default_gravity();
	}
}

void JoltArea3D::set_transform(Transform3D p_transform) {
	JOLT_ENSURE_SCALE_NOT_ZERO(p_transform, vformat("An invalid transform was passed to area '%s'.", to_string()));

	Vector3 new_scale;
	JoltMath::decompose(p_transform, new_scale);

	// Ideally we would do an exact comparison here, but due to floating-point precision this would be invalidated very often.
	if (!scale.is_equal_approx(new_scale)) {
		scale = new_scale;
		_shapes_changed();
	}

	if (!in_space()) {
		jolt_settings->mPosition = to_jolt_r(p_transform.origin);
		jolt_settings->mRotation = to_jolt(p_transform.basis);
	} else {
		space->get_body_iface().SetPositionAndRotation(jolt_body->GetID(), to_jolt_r(p_transform.origin), to_jolt(p_transform.basis), JPH::EActivation::DontActivate);
	}
}

Variant JoltArea3D::get_param(PhysicsServer3D::AreaParameter p_param) const {
	switch (p_param) {
		case PhysicsServer3D::AREA_PARAM_GRAVITY_OVERRIDE_MODE: {
			return get_gravity_mode();
		}
		case PhysicsServer3D::AREA_PARAM_GRAVITY: {
			return get_gravity();
		}
		case PhysicsServer3D::AREA_PARAM_GRAVITY_VECTOR: {
			return get_gravity_vector();
		}
		case PhysicsServer3D::AREA_PARAM_GRAVITY_IS_POINT: {
			return is_point_gravity();
		}
		case PhysicsServer3D::AREA_PARAM_GRAVITY_POINT_UNIT_DISTANCE: {
			return get_point_gravity_distance();
		}
		case PhysicsServer3D::AREA_PARAM_LINEAR_DAMP_OVERRIDE_MODE: {
			return get_linear_damp_mode();
		}
		case PhysicsServer3D::AREA_PARAM_LINEAR_DAMP: {
			return get_linear_damp();
		}
		case PhysicsServer3D::AREA_PARAM_ANGULAR_DAMP_OVERRIDE_MODE: {
			return get_angular_damp_mode();
		}
		case PhysicsServer3D::AREA_PARAM_ANGULAR_DAMP: {
			return get_angular_damp();
		}
		case PhysicsServer3D::AREA_PARAM_PRIORITY: {
			return get_priority();
		}
		case PhysicsServer3D::AREA_PARAM_WIND_FORCE_MAGNITUDE: {
			return AREA_DEFAULT_WIND_MAGNITUDE;
		}
		case PhysicsServer3D::AREA_PARAM_WIND_SOURCE: {
			return AREA_DEFAULT_WIND_SOURCE;
		}
		case PhysicsServer3D::AREA_PARAM_WIND_DIRECTION: {
			return AREA_DEFAULT_WIND_DIRECTION;
		}
		case PhysicsServer3D::AREA_PARAM_WIND_ATTENUATION_FACTOR: {
			return AREA_DEFAULT_WIND_ATTENUATION;
		}
		default: {
			ERR_FAIL_V_MSG(Variant(), vformat("Unhandled area parameter: '%d'. This should not happen. Please report this.", p_param));
		}
	}
}

void JoltArea3D::set_param(PhysicsServer3D::AreaParameter p_param, const Variant &p_value) {
	switch (p_param) {
		case PhysicsServer3D::AREA_PARAM_GRAVITY_OVERRIDE_MODE: {
			set_gravity_mode((OverrideMode)(int)p_value);
		} break;
		case PhysicsServer3D::AREA_PARAM_GRAVITY: {
			set_gravity(p_value);
		} break;
		case PhysicsServer3D::AREA_PARAM_GRAVITY_VECTOR: {
			set_gravity_vector(p_value);
		} break;
		case PhysicsServer3D::AREA_PARAM_GRAVITY_IS_POINT: {
			set_point_gravity(p_value);
		} break;
		case PhysicsServer3D::AREA_PARAM_GRAVITY_POINT_UNIT_DISTANCE: {
			set_point_gravity_distance(p_value);
		} break;
		case PhysicsServer3D::AREA_PARAM_LINEAR_DAMP_OVERRIDE_MODE: {
			set_linear_damp_mode((OverrideMode)(int)p_value);
		} break;
		case PhysicsServer3D::AREA_PARAM_LINEAR_DAMP: {
			set_area_linear_damp(p_value);
		} break;
		case PhysicsServer3D::AREA_PARAM_ANGULAR_DAMP_OVERRIDE_MODE: {
			set_angular_damp_mode((OverrideMode)(int)p_value);
		} break;
		case PhysicsServer3D::AREA_PARAM_ANGULAR_DAMP: {
			set_area_angular_damp(p_value);
		} break;
		case PhysicsServer3D::AREA_PARAM_PRIORITY: {
			set_priority(p_value);
		} break;
		case PhysicsServer3D::AREA_PARAM_WIND_FORCE_MAGNITUDE: {
			if (!Math::is_equal_approx((double)p_value, AREA_DEFAULT_WIND_MAGNITUDE)) {
				WARN_PRINT(vformat("Invalid wind force magnitude for '%s'. Area wind force magnitude is not supported when using Jolt Physics. Any such value will be ignored.", to_string()));
			}
		} break;
		case PhysicsServer3D::AREA_PARAM_WIND_SOURCE: {
			if (!((Vector3)p_value).is_equal_approx(AREA_DEFAULT_WIND_SOURCE)) {
				WARN_PRINT(vformat("Invalid wind source for '%s'. Area wind source is not supported when using Jolt Physics. Any such value will be ignored.", to_string()));
			}
		} break;
		case PhysicsServer3D::AREA_PARAM_WIND_DIRECTION: {
			if (!((Vector3)p_value).is_equal_approx(AREA_DEFAULT_WIND_DIRECTION)) {
				WARN_PRINT(vformat("Invalid wind direction for '%s'. Area wind direction is not supported when using Jolt Physics. Any such value will be ignored.", to_string()));
			}
		} break;
		case PhysicsServer3D::AREA_PARAM_WIND_ATTENUATION_FACTOR: {
			if (!Math::is_equal_approx((double)p_value, AREA_DEFAULT_WIND_ATTENUATION)) {
				WARN_PRINT(vformat("Invalid wind attenuation for '%s'. Area wind attenuation is not supported when using Jolt Physics. Any such value will be ignored.", to_string()));
			}
		} break;
		default: {
			ERR_FAIL_MSG(vformat("Unhandled area parameter: '%d'. This should not happen. Please report this.", p_param));
		} break;
	}
}

void JoltArea3D::set_body_monitor_callback(const Callable &p_callback) {
	if (p_callback == body_monitor_callback) {
		return;
	}

	body_monitor_callback = p_callback;

	_body_monitoring_changed();
}

void JoltArea3D::set_area_monitor_callback(const Callable &p_callback) {
	if (p_callback == area_monitor_callback) {
		return;
	}

	area_monitor_callback = p_callback;

	_area_monitoring_changed();
}

void JoltArea3D::set_monitorable(bool p_monitorable) {
	if (p_monitorable == monitorable) {
		return;
	}

	monitorable = p_monitorable;

	_monitorable_changed();
}

bool JoltArea3D::can_monitor(const JoltBody3D &p_other) const {
	return is_monitoring_bodies() && (collision_mask & p_other.get_collision_layer()) != 0;
}

bool JoltArea3D::can_monitor(const JoltSoftBody3D &p_other) const {
	return false;
}

bool JoltArea3D::can_monitor(const JoltArea3D &p_other) const {
	return is_monitoring_areas() && p_other.is_monitorable() && (collision_mask & p_other.get_collision_layer()) != 0;
}

bool JoltArea3D::can_interact_with(const JoltBody3D &p_other) const {
	return can_monitor(p_other);
}

bool JoltArea3D::can_interact_with(const JoltSoftBody3D &p_other) const {
	return false;
}

bool JoltArea3D::can_interact_with(const JoltArea3D &p_other) const {
	return can_monitor(p_other) || p_other.can_monitor(*this);
}

Vector3 JoltArea3D::get_velocity_at_position(const Vector3 &p_position) const {
	return Vector3();
}

void JoltArea3D::set_point_gravity(bool p_enabled) {
	if (point_gravity == p_enabled) {
		return;
	}

	point_gravity = p_enabled;

	_gravity_changed();
}

void JoltArea3D::set_gravity(float p_gravity) {
	if (gravity == p_gravity) {
		return;
	}

	gravity = p_gravity;

	_gravity_changed();
}

void JoltArea3D::set_point_gravity_distance(float p_distance) {
	if (point_gravity_distance == p_distance) {
		return;
	}

	point_gravity_distance = p_distance;

	_gravity_changed();
}

void JoltArea3D::set_gravity_mode(OverrideMode p_mode) {
	if (gravity_mode == p_mode) {
		return;
	}

	gravity_mode = p_mode;

	_gravity_changed();
}

void JoltArea3D::set_gravity_vector(const Vector3 &p_vector) {
	if (gravity_vector == p_vector) {
		return;
	}

	gravity_vector = p_vector;

	_gravity_changed();
}

Vector3 JoltArea3D::compute_gravity(const Vector3 &p_position) const {
	if (!point_gravity) {
		return gravity_vector * gravity;
	}

	const Vector3 point = get_transform_scaled().xform(gravity_vector);
	const Vector3 to_point = point - p_position;
	const real_t to_point_dist_sq = MAX(to_point.length_squared(), (real_t)CMP_EPSILON);
	const Vector3 to_point_dir = to_point / Math::sqrt(to_point_dist_sq);

	if (point_gravity_distance == 0.0f) {
		return to_point_dir * gravity;
	}

	const float gravity_dist_sq = point_gravity_distance * point_gravity_distance;

	return to_point_dir * (gravity * gravity_dist_sq / to_point_dist_sq);
}

void JoltArea3D::body_shape_entered(const JPH::BodyID &p_body_id, const JPH::SubShapeID &p_other_shape_id, const JPH::SubShapeID &p_self_shape_id) {
	Overlap &overlap = bodies_by_id[p_body_id];

	if (overlap.shape_pairs.is_empty()) {
		_notify_body_entered(p_body_id);
	}

	_add_shape_pair(overlap, p_body_id, p_other_shape_id, p_self_shape_id);
}

bool JoltArea3D::body_shape_exited(const JPH::BodyID &p_body_id, const JPH::SubShapeID &p_other_shape_id, const JPH::SubShapeID &p_self_shape_id) {
	Overlap *overlap = bodies_by_id.getptr(p_body_id);
	if (overlap == nullptr) {
		return false;
	}

	if (!_remove_shape_pair(*overlap, p_other_shape_id, p_self_shape_id)) {
		return false;
	}

	if (overlap->shape_pairs.is_empty()) {
		_notify_body_exited(p_body_id);
	}

	return true;
}

void JoltArea3D::area_shape_entered(const JPH::BodyID &p_body_id, const JPH::SubShapeID &p_other_shape_id, const JPH::SubShapeID &p_self_shape_id) {
	_add_shape_pair(areas_by_id[p_body_id], p_body_id, p_other_shape_id, p_self_shape_id);
}

bool JoltArea3D::area_shape_exited(const JPH::BodyID &p_body_id, const JPH::SubShapeID &p_other_shape_id, const JPH::SubShapeID &p_self_shape_id) {
	Overlap *overlap = areas_by_id.getptr(p_body_id);
	if (overlap == nullptr) {
		return false;
	}

	return _remove_shape_pair(*overlap, p_other_shape_id, p_self_shape_id);
}

bool JoltArea3D::shape_exited(const JPH::BodyID &p_body_id, const JPH::SubShapeID &p_other_shape_id, const JPH::SubShapeID &p_self_shape_id) {
	return body_shape_exited(p_body_id, p_other_shape_id, p_self_shape_id) || area_shape_exited(p_body_id, p_other_shape_id, p_self_shape_id);
}

void JoltArea3D::body_exited(const JPH::BodyID &p_body_id, bool p_notify) {
	Overlap *overlap = bodies_by_id.getptr(p_body_id);
	if (unlikely(overlap == nullptr)) {
		return;
	}

	if (unlikely(overlap->shape_pairs.is_empty())) {
		return;
	}

	for (const KeyValue<ShapeIDPair, ShapeIndexPair> &E : overlap->shape_pairs) {
		overlap->pending_added.erase(E.value);
		overlap->pending_removed.push_back(E.value);
	}

	_events_changed();

	overlap->shape_pairs.clear();

	if (p_notify) {
		_notify_body_exited(p_body_id);
	}
}

void JoltArea3D::area_exited(const JPH::BodyID &p_body_id) {
	Overlap *overlap = areas_by_id.getptr(p_body_id);
	if (unlikely(overlap == nullptr)) {
		return;
	}

	if (unlikely(overlap->shape_pairs.is_empty())) {
		return;
	}

	for (const KeyValue<ShapeIDPair, ShapeIndexPair> &E : overlap->shape_pairs) {
		overlap->pending_added.erase(E.value);
		overlap->pending_removed.push_back(E.value);
	}

	_events_changed();

	overlap->shape_pairs.clear();
}

void JoltArea3D::call_queries() {
	_flush_events(bodies_by_id, body_monitor_callback);
	_flush_events(areas_by_id, area_monitor_callback);
}
