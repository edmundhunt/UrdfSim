// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "UnrealLidarSensor.h"
#include "AirBlueprintLib.h"
#include "common/Common.hpp"
#include "NedTransform.h"
#include "DrawDebugHelpers.h"
#include "Vehicles/UrdfBot/UrdfLink.h"

// ctor
UnrealLidarSensor::UnrealLidarSensor(const AirSimSettings::LidarSetting& setting,
    AActor* actor, const NedTransform* ned_transform)
    : LidarSimple(setting), actor_(actor), ned_transform_(ned_transform)
{
    msr::airlib::LidarSimpleParams params = getParams();

    createLasers(params);

    this->ignore_pawn_collision_ = params.ignore_pawn_collision;
    this->draw_debug_points_ = params.draw_debug_points;
    this->ignore_collision_actors_ = TArray<const AActor*>();

    // This is a bit of a hack
    // The problem is that for URDF bot, we oftentimes want to spawn the lidar inside geometry
    //   (for example, the LIDAR is usually enclosed inside a plastic case, which is represented by static geometry.)
    // In this case, we want to ignore collision with both the case it's involved in, and with the enclosing "UrdfBot" component. 
    // We want to maintain collision with other parts of the bot, e.g. an arm if the bot is mounted on the  base. 
    // 
    // This solution creates a bit of a coupling between the UrdfBot and the Lidar.
    // Ideally, there should be some sort of "GetGroupedCollisionComponents", but that seems like overkill. 
    if (this->ignore_pawn_collision_)
    {
        AUrdfLink* link = dynamic_cast<AUrdfLink*>(this->actor_);
        if (link != nullptr)
        {
            AActor* owner = static_cast<AActor*>(link->GetOwningActor());
            this->ignore_collision_actors_.Add(owner);
        }
    }
}

// initializes information based on lidar configuration
void UnrealLidarSensor::createLasers(msr::airlib::LidarSimpleParams params)
{
    const auto number_of_lasers = params.number_of_channels;

    if (number_of_lasers <= 0)
        return;

    // calculate verticle angle distance between each laser
    if (number_of_lasers == 1)
    {
        laser_angles_.clear();
        laser_angles_.emplace_back(0);
        return;
    }

    const float delta_angle =
        (params.vertical_FOV_upper - (params.vertical_FOV_lower)) /
        static_cast<float>(number_of_lasers - 1);

    // store vertical angles for each laser
    laser_angles_.clear();
    for (auto i = 0u; i < number_of_lasers; ++i)
    {
        const float vertical_angle = params.vertical_FOV_upper - static_cast<float>(i) * delta_angle;
        laser_angles_.emplace_back(vertical_angle);
    }
}

// returns a point-cloud for the tick
void UnrealLidarSensor::getPointCloud(const msr::airlib::Pose& lidar_pose, const msr::airlib::Pose& vehicle_pose,
    const msr::airlib::TTimeDelta delta_time, msr::airlib::vector<msr::airlib::real_T>& point_cloud)
{
    point_cloud.clear();

    msr::airlib::LidarSimpleParams params = getParams();
    const auto number_of_lasers = params.number_of_channels;

    // cap the points to scan via ray-tracing; this is currently needed for car/Unreal tick scenarios
    // since SensorBase mechanism uses the elapsed clock time instead of the tick delta-time.
    constexpr float MAX_POINTS_IN_SCAN = 1e+5f;
    uint32 total_points_to_scan = FMath::RoundHalfFromZero(params.points_per_second * delta_time);
    if (total_points_to_scan > MAX_POINTS_IN_SCAN)
    {
        total_points_to_scan = MAX_POINTS_IN_SCAN;
        UAirBlueprintLib::LogMessageString("Lidar: ", "Capping number of points to scan", LogDebugLevel::Failure);
    }

    // calculate number of points needed for each laser/channel
    const uint32 points_to_scan_with_one_laser = FMath::RoundHalfFromZero(total_points_to_scan / float(number_of_lasers));
    if (points_to_scan_with_one_laser <= 0)
    {
        //UAirBlueprintLib::LogMessageString("Lidar: ", "No points requested this frame", LogDebugLevel::Failure);
        return;
    }

    // calculate needed angle/distance between each point
    const float angle_distance_of_tick = params.horizontal_rotation_frequency * 360.0f * delta_time;
    const float angle_distance_of_laser_measure = angle_distance_of_tick / points_to_scan_with_one_laser;

    // start position
    Vector3r start = vehicle_pose.position;
    Vector3r lidar_offset = lidar_pose.position;

    lidar_offset = VectorMath::rotateVector(lidar_offset, vehicle_pose.orientation, true);

    start += lidar_offset;

    if (this->draw_debug_points_)
    {
        DrawDebugPoint(
            actor_->GetWorld(),
            FVector(start.x(), start.y(), start.z()),
            5,                       //size
            FColor::Blue,
            false,                    //persistent (never goes away)
            0.1                      //point leaves a trail on moving object
        );
    }

    // shoot lasers
    for (auto laser = 0u; laser < number_of_lasers; ++laser)
    {
        for (auto i = 0u; i < points_to_scan_with_one_laser; ++i)
        {
            Vector3r point;
            const float angle = current_horizontal_angle_ + angle_distance_of_laser_measure * i;
            // shoot laser and get the impact point, if any
            if (shootLaser(lidar_pose, vehicle_pose, start, laser, angle, params, point))
            {
                point_cloud.emplace_back(point.x());
                point_cloud.emplace_back(point.y());
                point_cloud.emplace_back(point.z());
            }
        }
    }

    current_horizontal_angle_ = std::fmod(current_horizontal_angle_ + angle_distance_of_tick, 360.0f);

    return;
}

// simulate shooting a laser via Unreal ray-tracing.
bool UnrealLidarSensor::shootLaser(const msr::airlib::Pose& lidar_pose, const msr::airlib::Pose& vehicle_pose, msr::airlib::Vector3r start,
    const uint32 laser, const float horizontalAngle, msr::airlib::LidarSimpleParams params, Vector3r &point)
{
    const float vertical_angle = laser_angles_[laser];

    msr::airlib::Vector3r end = msr::airlib::Vector3r::Zero();
    FVector endVec = FVector::ZeroVector;
    FVector startVec(start.x(), start.y(), start.z());
    FRotator rayRotator;
    FRotator poseRotator;
    FRotator actorRotator;

    if (this->ned_transform_ == nullptr)
    {
        rayRotator = FRotator(vertical_angle, horizontalAngle, 0);
        poseRotator = FQuat(lidar_pose.orientation.x(), lidar_pose.orientation.y(), lidar_pose.orientation.z(), lidar_pose.orientation.w()).Rotator();
        actorRotator = this->actor_->GetActorRotation();
        endVec = FVector(1, 0, 0);
        endVec = rayRotator.RotateVector(endVec);
        endVec = poseRotator.RotateVector(endVec);
        endVec = actorRotator.RotateVector(endVec);
        endVec = (endVec * params.range) + startVec;
        end = msr::airlib::Vector3r(endVec.X, endVec.Y, endVec.Z);
    }
    else
    {
        // get ray quaternion in lidar frame (angles must be in radians)
        // This does strange things with the UrdfBot. So use unreal libraries for UrdfBot and tested algorithm for car / drone.
        msr::airlib::Quaternionr ray_q_l = msr::airlib::VectorMath::toQuaternion(
            msr::airlib::Utils::degreesToRadians(vertical_angle),   //pitch - rotation around Y axis
            0,                                                      //roll  - rotation around X axis
            msr::airlib::Utils::degreesToRadians(horizontalAngle)); //yaw   - rotation around Z axis

        auto ray_q_l_o = ray_q_l.toRotationMatrix().eulerAngles(0, 1, 2);
        auto ray_q_l_o_x = ray_q_l_o.x();
        auto ray_q_l_o_y = ray_q_l_o.y();
        auto ray_q_l_o_z = ray_q_l_o.z();

        // get ray quaternion in body frame
        msr::airlib::Quaternionr ray_q_b = VectorMath::rotateQuaternion(ray_q_l, lidar_pose.orientation, true);
        auto ray_q_b_o = ray_q_b.toRotationMatrix().eulerAngles(0, 1, 2);
        auto ray_q_b_o_x = FMath::RadiansToDegrees(ray_q_b_o.x());
        auto ray_q_b_o_y = FMath::RadiansToDegrees(ray_q_b_o.y());
        auto ray_q_b_o_z = FMath::RadiansToDegrees(ray_q_b_o.z());

        // get ray quaternion in world frame
        msr::airlib::Quaternionr www(-vehicle_pose.orientation.w(), -vehicle_pose.orientation.x(), vehicle_pose.orientation.y(), vehicle_pose.orientation.z());
        msr::airlib::Quaternionr ray_q_w = VectorMath::rotateQuaternion(ray_q_b, vehicle_pose.orientation, true);

        //get ray vector (end position)
        Vector3r end = VectorMath::rotateVector(VectorMath::front(), ray_q_w, true) * params.range + start;
        endVec = FVector(end.x(), end.y(), end.z());
    }
    
    FHitResult hit_result = FHitResult(ForceInit);

    if (this->ned_transform_ != nullptr)
    {
        startVec = ned_transform_->fromLocalNed(start);
        endVec = ned_transform_->fromLocalNed(end);
    }

    FVector shootVec = endVec - startVec;

    double len = shootVec.Size();

    if (len > 601 || len < 599)
    {
        int j = 0;
    }

    bool is_hit = UAirBlueprintLib::GetObstacle(actor_, startVec, endVec, hit_result, this->ignore_collision_actors_, ECC_Visibility, this->ignore_pawn_collision_);

    if (is_hit)
    {
        if (true && UAirBlueprintLib::IsInGameThread())
        {
           
            if (this->draw_debug_points_)
            {
                DrawDebugPoint(
                    actor_->GetWorld(),
                    hit_result.ImpactPoint,
                    5,                       //size
                    FColor::Red,
                    false,                    //persistent (never goes away)
                    0.1                      //point leaves a trail on moving object
                );
            }
          
        }

        if (this->ned_transform_ != nullptr)
        {
            point = ned_transform_->toLocalNed(hit_result.ImpactPoint);
        }
        else
        {
            // Translate difference vector back to actor frame
            FVector diffVec = hit_result.ImpactPoint - startVec;
            FVector inActor = actorRotator.UnrotateVector(diffVec);
            FVector inPose = poseRotator.UnrotateVector(inActor);

            point = msr::airlib::Vector3r(inPose.X, inPose.Y, inPose.Z);
        }
       
        return true;
    }
    else 
    {
        if (this->draw_debug_points_)
        {
            DrawDebugPoint(
                actor_->GetWorld(),
                endVec,
                5,                       //size
                FColor::Green,
                false,                    //persistent (never goes away)
                0.1                      //point leaves a trail on moving object
            );
        }

        // Use end point as the result
        FVector diffVec = endVec - startVec;
        FVector inActor = actorRotator.UnrotateVector(diffVec);
        FVector inPose = poseRotator.UnrotateVector(inActor);

        point = msr::airlib::Vector3r(inPose.X, inPose.Y, inPose.Z);

        return true;
        // return false;
    }
}
