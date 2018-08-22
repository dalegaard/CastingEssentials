#include "Modules/Camera/SimpleCameraSmooth.h"
#include "PluginBase/Interfaces.h"

#include <cdll_int.h>
#include <con_nprint.h>
#include <convar.h>

#include <algorithm>

#undef min

SimpleCameraSmooth::SimpleCameraSmooth(CameraPtr&& startCamera, CameraPtr&& endCamera, float duration) :
	m_StartCamera(std::move(startCamera)), m_EndCamera(std::move(endCamera)), m_Duration(duration)
{
	const auto startCam = GetStartCamera();
	m_Origin = startCam->GetOrigin();
	m_Angles = startCam->GetAngles();
	m_FOV = startCam->GetFOV();

	Assert(std::isfinite(m_Duration));
}

void SimpleCameraSmooth::Reset()
{
	m_CurrentTime = 0;
}

static ConVar wtf_invert("wtf_invert", "0");

void SimpleCameraSmooth::Update(float dt, uint32_t frame)
{
	m_StartCamera->TryUpdate(dt, frame);
	TryCollapse(m_StartCamera);
	m_EndCamera->TryUpdate(dt, frame);
	TryCollapse(m_EndCamera);

	const float previousProgressLinear = GetProgress();
	const float previousProgress = m_Interpolator(previousProgressLinear);

	if (!IsSmoothComplete())
	{
		if (m_CurrentTime <= 0)
		{
			m_StartAngles = m_Angles = m_StartCamera->GetAngles();
			m_StartOrigin = m_Origin = m_StartCamera->GetOrigin();
			m_StartFOV = m_FOV = m_StartCamera->GetFOV();

			//m_InitialStartAngles = Quaternion(m_StartCamera->GetAngles());
			//m_InitialEndAngles = Quaternion(m_EndCamera->GetAngles());

			//m_InitialAngleDot = QuaternionDotProduct(m_InitialStartAngles, m_InitialEndAngles);

			m_PreviousAngles = Quaternion(m_StartAngles);
		}

		m_CurrentTime = std::min(m_CurrentTime + dt, m_Duration);
	}

	const float progressLinear = GetProgress();
	const float progress = m_Interpolator(progressLinear);
	float progressDelta = 1;
	if (previousProgress < 1)
		progressDelta = (progress - previousProgress) / (1 - previousProgress);

	if (m_UpdateStartOrigin)
		m_Origin = Lerp(progress, m_StartCamera->GetOrigin(), m_EndCamera->GetOrigin());
	else
		m_Origin = Lerp(progress, m_StartOrigin, m_EndCamera->GetOrigin());

	if (m_UpdateStartAngles)
	{
		// Only two possible ways to slerp
		Quaternion shortWay, longWay;

		const Quaternion startQuat(m_StartCamera->GetAngles());

		Quaternion endQuat(m_EndCamera->GetAngles());
		QuaternionAlign(startQuat, endQuat, endQuat);

		QuaternionSlerpNoAlign(startQuat, endQuat, progress, shortWay);
		QuaternionSlerpNoAlign(startQuat, -endQuat, progress, longWay);
		//QuaternionBlendNoAlign(startQuat, endQuat, progress, shortWay);
		//QuaternionBlendNoAlign(startQuat, -endQuat, 1 - progress, longWay);

		QuaternionNormalize(shortWay);
		QuaternionNormalize(longWay);

		auto diffShort = std::fabsf(QuaternionAngleDiff(m_PreviousAngles, shortWay));
		auto diffLong = std::fabsf(QuaternionAngleDiff(m_PreviousAngles, longWay));

		auto engine = Interfaces::GetEngineClient();

		con_nprint_s printData;
		printData.color[0] = printData.color[1] = printData.color[2] = 1;
		printData.fixed_width_font = false;
		printData.time_to_live = -1;

		engine->Con_NXPrintf(GetConLine(printData), "");
		engine->Con_NXPrintf(GetConLine(printData), "angles start: %1.2f %1.2f %1.2f %1.2f",
			startQuat.x, startQuat.y, startQuat.z, startQuat.w);
		engine->Con_NXPrintf(GetConLine(printData), "angles cur: %1.2f %1.2f %1.2f %1.2f",
			m_PreviousAngles.x, m_PreviousAngles.y, m_PreviousAngles.z, m_PreviousAngles.w);
		engine->Con_NXPrintf(GetConLine(printData), "angles end: %1.2f %1.2f %1.2f %1.2f",
			endQuat.x, endQuat.y, endQuat.z, endQuat.w);
		engine->Con_NXPrintf(GetConLine(printData), "diffShort: %1.2f", diffShort);
		engine->Con_NXPrintf(GetConLine(printData), "diffLong: %1.2f", diffLong);

		QAngle angShort, angLong;
		QuaternionAngles(shortWay, angShort);
		QuaternionAngles(longWay, angLong);

		RotationDelta(m_Angles, angShort, &angShort);
		RotationDelta(m_Angles, angLong, &angLong);

		//diffShort = std::fabsf(angShort.x) + std::fabsf(angShort.y) + std::fabsf(angShort.z);
		//diffLong = std::fabsf(angLong.x) + std::fabsf(angLong.y) + std::fabsf(angLong.z);

		//diffShort = QuaternionAngleDist(m_PreviousAngles, shortWay);
		//diffLong = QuaternionAngleDist(m_PreviousAngles, longWay);

		//engine->Con_NXPrintf(GetConLine(printData), "diffShort manual: %1.2f", Rad2Deg(diffShort));
		//engine->Con_NXPrintf(GetConLine(printData), "diffLong manual: %1.2f", Rad2Deg(diffLong));

		if (diffShort <= diffLong + 1)
			m_PreviousAngles = shortWay;
		else
			m_PreviousAngles = longWay;

		QuaternionAngles(m_PreviousAngles, m_Angles);
	}
	else
		m_Angles = Lerp(progressDelta, m_Angles, m_EndCamera->GetAngles());

	if (m_UpdateStartFOV)
		m_FOV = Lerp(progress, m_StartCamera->GetFOV(), m_EndCamera->GetFOV());
	else
		m_FOV = Lerp(progress, m_StartFOV, m_EndCamera->GetFOV());

	if (progress == 0)
		m_IsFirstPerson = m_StartCamera->IsFirstPerson();
	else if (progress == 1)
		m_IsFirstPerson = m_EndCamera->IsFirstPerson();
	else
		m_IsFirstPerson = false;

	Assert(m_Origin.IsValid());
	Assert(m_Angles.IsValid());
	Assert(std::isfinite(m_FOV));
}

Quaternion SimpleCameraSmooth::QuaternionDelta(const Quaternion& a, const Quaternion& b)
{
	return Quaternion(b.x - a.x, b.y - a.y, b.z - a.z, b.w - a.w);
}

Quaternion SimpleCameraSmooth::HackQuaternionScale(const Quaternion& a, float scalar)
{
	return Quaternion(
		a.x * scalar,
		a.y * scalar,
		a.z * scalar,
		a.w * scalar
	);
}

Quaternion SimpleCameraSmooth::HackQuaternionSlerp(const Quaternion& a, Quaternion b, float t)
{
	Assert(a.IsValid());
	Assert(b.IsValid());

	Assert(AlmostEqual(QuaternionDotProduct(a, a), 1) || AlmostEqual(QuaternionDotProduct(a, a), 0));
	Assert(AlmostEqual(QuaternionDotProduct(b, b), 1) || AlmostEqual(QuaternionDotProduct(b, b), 0));

	auto dot = QuaternionDotProduct(a, b);

	if (dot < 0)
	{
		for (int i = 0; i < 4; i++)
			b[i] = -b[i];

		dot = -dot;
	}

	Quaternion retVal;
	if (dot > 0.000001f)
	{
		// Too close for comfort
		for (int i = 0; i < 4; i++)
			retVal[i] = Lerp(a[i], b[i], t);
	}
	else
	{
		// Since dot is in range [0, DOT_THRESHOLD], acos is safe
		double theta_0 = acos(dot);        // theta_0 = angle between input vectors
		double theta = theta_0 * t;          // theta = angle between v0 and result
		double sin_theta = sin(theta);     // compute this value only once
		double sin_theta_0 = sin(theta_0); // compute this value only once

		double s0 = cos(theta) - dot * sin_theta / sin_theta_0;  // == sin(theta_0 - theta) / sin(theta_0)
		double s1 = sin_theta / sin_theta_0;

		for (int i = 0; i < 4; i++)
			retVal[i] = (s0 * a[i]) + (s1 * b[i]);
	}

	Assert(retVal.IsValid());
	retVal.NormalizeInPlace();
	return retVal;
}

float SimpleCameraSmooth::QuaternionAngleDist(const Quaternion& a, const Quaternion& b)
{
	Assert(a.IsValid());
	Assert(b.IsValid());

	Assert(AlmostEqual(QuaternionDotProduct(a, a), 1) || AlmostEqual(QuaternionDotProduct(a, a), 0));
	Assert(AlmostEqual(QuaternionDotProduct(b, b), 1) || AlmostEqual(QuaternionDotProduct(b, b), 0));

	// https://math.stackexchange.com/a/90098/329605
	auto innerProduct = QuaternionDotProduct(a, b);
	Assert((innerProduct > -1 && innerProduct < 1) || AlmostEqual(innerProduct, 1) || AlmostEqual(innerProduct, -1));

	float innerProductClamped = std::clamp<float>(innerProduct, -1, 1);
	auto retVal = 2 * std::acosf(innerProductClamped);

	Assert(std::isfinite(retVal));
	return retVal;
}