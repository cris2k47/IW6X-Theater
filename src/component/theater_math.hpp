#pragma once

// Small math library for the theater cameras (Quake/IW conventions: X forward, Y left, Z up;
// angles are {pitch, yaw, roll} in degrees, positive pitch looks down).

namespace theater::math
{
	constexpr double PI = 3.14159265358979323846;

	inline double deg2rad(const double d) { return d * PI / 180.0; }
	inline double rad2deg(const double r) { return r * 180.0 / PI; }

	struct vec3
	{
		double x{}, y{}, z{};

		vec3 operator+(const vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
		vec3 operator-(const vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
		vec3 operator*(const double s) const { return {x * s, y * s, z * s}; }
		vec3& operator+=(const vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
	};

	inline double dot(const vec3& a, const vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline double length(const vec3& a) { return std::sqrt(dot(a, a)); }

	struct quat
	{
		double w{1}, x{}, y{}, z{};

		quat operator*(const quat& b) const
		{
			return {
				w * b.w - x * b.x - y * b.y - z * b.z,
				w * b.x + x * b.w + y * b.z - z * b.y,
				w * b.y - x * b.z + y * b.w + z * b.x,
				w * b.z + x * b.y - y * b.x + z * b.w,
			};
		}

		quat operator-() const { return {-w, -x, -y, -z}; }
	};

	inline double dot(const quat& a, const quat& b) { return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z; }

	inline quat normalize(const quat& q)
	{
		const auto len = std::sqrt(dot(q, q));
		if (len < 1e-12) return {};
		return {q.w / len, q.x / len, q.y / len, q.z / len};
	}

	inline quat axis_angle(const vec3& axis, const double rad)
	{
		const auto s = std::sin(rad * 0.5);
		return {std::cos(rad * 0.5), axis.x * s, axis.y * s, axis.z * s};
	}

	// R = Rz(yaw) * Ry(pitch) * Rx(roll)  ==  AnglesToAxis(angles)
	inline quat from_angles(const double pitch, const double yaw, const double roll)
	{
		const auto qz = axis_angle({0, 0, 1}, deg2rad(yaw));
		const auto qy = axis_angle({0, 1, 0}, deg2rad(pitch));
		const auto qx = axis_angle({1, 0, 0}, deg2rad(roll));
		return normalize(qz * qy * qx);
	}

	inline vec3 rotate(const quat& q, const vec3& v)
	{
		const quat p{0, v.x, v.y, v.z};
		const quat qc{q.w, -q.x, -q.y, -q.z};
		const auto r = q * p * qc;
		return {r.x, r.y, r.z};
	}

	// axis[0] = forward, axis[1] = left, axis[2] = up
	inline void to_axis(const quat& q, vec3 axis[3])
	{
		axis[0] = rotate(q, {1, 0, 0});
		axis[1] = rotate(q, {0, 1, 0});
		axis[2] = rotate(q, {0, 0, 1});
	}

	// angles of a view axis (forward, left, up), the inverse of AnglesToAxis
	inline void axis_to_angles(const vec3 axis[3], double& pitch, double& yaw, double& roll)
	{
		const auto& f = axis[0];
		const auto& l = axis[1];
		const auto& u = axis[2];

		pitch = rad2deg(std::asin(std::clamp(-f.z, -1.0, 1.0)));
		yaw = rad2deg(std::atan2(f.y, f.x));
		roll = rad2deg(std::atan2(l.z, u.z));
	}

	inline void to_angles(const quat& q, double& pitch, double& yaw, double& roll)
	{
		vec3 axis[3];
		to_axis(q, axis);
		const auto& f = axis[0];
		const auto& l = axis[1];
		const auto& u = axis[2];

		pitch = rad2deg(std::asin(std::clamp(-f.z, -1.0, 1.0)));
		yaw = rad2deg(std::atan2(f.y, f.x));
		roll = rad2deg(std::atan2(l.z, u.z));
	}

	inline quat slerp(const quat& a, quat b, const double t)
	{
		auto cos_theta = dot(a, b);
		if (cos_theta < 0.0)
		{
			b = -b;
			cos_theta = -cos_theta;
		}

		if (cos_theta > 0.9995)
		{
			return normalize({
				a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t,
				a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
			});
		}

		const auto theta = std::acos(std::clamp(cos_theta, -1.0, 1.0));
		const auto sin_theta = std::sin(theta);
		const auto wa = std::sin((1.0 - t) * theta) / sin_theta;
		const auto wb = std::sin(t * theta) / sin_theta;
		return normalize({wa * a.w + wb * b.w, wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z});
	}

	// ------------------------------------------------------------------ non-uniform Catmull-Rom
	// Barry-Goldman pyramidal formulation with the keyframe times as knots: the curve passes through
	// every key exactly at its time and velocity is continuous across keys. 'mix' is the two-point
	// blend (lerp for vectors/scalars, slerp for rotations); parameters may lie outside [0, 1].

	template <typename T, typename Mix>
	T catmull_rom(const T& p0, const T& p1, const T& p2, const T& p3,
	              const double t0, const double t1, const double t2, const double t3, const double t, Mix mix)
	{
		auto blend = [&](const T& a, const T& b, const double ta, const double tb)
		{
			const auto span = tb - ta;
			const auto u = std::abs(span) < 1e-9 ? 0.0 : (t - ta) / span;
			return mix(a, b, u);
		};

		const auto a1 = blend(p0, p1, t0, t1);
		const auto a2 = blend(p1, p2, t1, t2);
		const auto a3 = blend(p2, p3, t2, t3);
		const auto b1 = blend(a1, a2, t0, t2);
		const auto b2 = blend(a2, a3, t1, t3);
		return blend(b1, b2, t1, t2);
	}

	inline vec3 lerp(const vec3& a, const vec3& b, const double u) { return a + (b - a) * u; }
	inline double lerp(const double a, const double b, const double u) { return a + (b - a) * u; }
}
