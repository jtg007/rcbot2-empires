#ifndef __RCBOT_LOCAL_CONFLICT_TEST_VECTOR_H__
#define __RCBOT_LOCAL_CONFLICT_TEST_VECTOR_H__

#include <cmath>

struct Vector
{
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;

	Vector() = default;
	Vector(float xValue, float yValue, float zValue)
		: x(xValue), y(yValue), z(zValue) {}

	Vector operator-(const Vector& other) const
	{
		return Vector(x - other.x, y - other.y, z - other.z);
	}

	Vector operator+(const Vector& other) const
	{
		return Vector(x + other.x, y + other.y, z + other.z);
	}

	Vector operator*(float scalar) const
	{
		return Vector(x * scalar, y * scalar, z * scalar);
	}

	float LengthSqr() const { return x * x + y * y + z * z; }
	float Length2D() const { return std::sqrt(x * x + y * y); }
	float Length() const { return std::sqrt(LengthSqr()); }
};

#endif // __RCBOT_LOCAL_CONFLICT_TEST_VECTOR_H__
