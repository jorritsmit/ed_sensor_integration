#include "fitter_plugin.h"

#include <ed/entity.h>
#include <ed/world_model.h>
#include <ed/update_request.h>
#include <geolib/Shape.h>

// Image capture
#include <rgbd/Image.h>
#include <geolib/ros/tf_conversions.h>

//
#include <rgbd/View.h>

// Visualization
#include <opencv2/highgui/highgui.hpp>

// 2D model creation
#include "mesh_tools.h"

// ----------------------------------------------------------------------------------------------------

// Decomposes 'pose' into a (X, Y, YAW) and (Z, ROLL, PITCH) component
void decomposePose(const geo::Pose3D& pose, geo::Pose3D& pose_xya, geo::Pose3D& pose_zrp)
{
    tf::Matrix3x3 m;
    geo::convert(pose.R, m);

    double roll, pitch, yaw;
    m.getRPY(roll, pitch, yaw);

    pose_xya.R.setRPY(0, 0, yaw);
    pose_xya.t = geo::Vec3(pose.t.x, pose.t.y, 0);

    pose_zrp = pose_xya.inverse() * pose;
}

// ----------------------------------------------------------------------------------------------------

// Convert a 3D transform with only a x, y and yaw component to a 2D transform
geo::Transform2 XYYawToTransform2(const geo::Pose3D& pose)
{
    return geo::Transform2(geo::Mat2(pose.R.xx, pose.R.xy, pose.R.yx, pose.R.yy), geo::Vec2(pose.t.x, pose.t.y));
}

// ----------------------------------------------------------------------------------------------------

FitterPlugin::FitterPlugin() : tf_listener_(0)
{
}

// ----------------------------------------------------------------------------------------------------

FitterPlugin::~FitterPlugin()
{
    delete tf_listener_;
}

// ----------------------------------------------------------------------------------------------------

void FitterPlugin::initialize(ed::InitData& init)
{
    tue::Configuration& config = init.config;
    rgbd_client_.intialize("/amigo/top_kinect/rgbd");

    beam_model_.initialize(2, 200);

    tf_listener_ = new tf::TransformListener;
}

// ----------------------------------------------------------------------------------------------------

void FitterPlugin::process(const ed::PluginInput& data, ed::UpdateRequest& req)
{
    const ed::WorldModel& world = data.world;

    // -------------------------------------
    // Grab image and sensor pose

    rgbd::ImageConstPtr image;
    geo::Pose3D sensor_pose;
    if (!NextImage("/map", image, sensor_pose))
        return;

    // -------------------------------------
    // Decompose sensor pose into X Y YAW and Z ROLL PITCH

    geo::Pose3D sensor_pose_xya;
    geo::Pose3D sensor_pose_zrp;
    decomposePose(sensor_pose, sensor_pose_xya, sensor_pose_zrp);

    // -------------------------------------
    // Calculate virtual rgbd beam ranges

    const cv::Mat& depth = image->getDepthImage();
    rgbd::View view(*image, depth.cols);

    std::vector<double> ranges(beam_model_.num_beams(), 0);

    for(int x = 0; x < depth.cols; ++x)
    {
        for(int y = 0; y < depth.rows; ++y)
        {
            float d = depth.at<float>(y, x);
            if (d == 0 || d != d)
                continue;

            geo::Vector3 p_sensor = view.getRasterizer().project2Dto3D(x, y) * d;
            geo::Vector3 p_floor = sensor_pose_zrp * p_sensor;

            if (p_floor.z < 0.2) // simple floor filter
                continue;

            int i = beam_model_.CalculateBeam(p_floor.x, p_floor.y);
            if (i >= 0 && i < ranges.size())
            {
                double& r = ranges[i];
                if (r == 0 || p_floor.y < r)
                    r = p_floor.y;
            }
        }
    }

    // -------------------------------------
    // Render world model objects

    geo::Transform2 sensor_pose_xya_2d = XYYawToTransform2(sensor_pose_xya);

    std::vector<double> model_ranges(ranges.size(), 0);

    cv::Mat canvas(600, 600, CV_8UC3, cv::Scalar(0, 0, 0));


    for(ed::WorldModel::const_iterator it = world.begin(); it != world.end(); ++it)
    {
        const ed::EntityConstPtr& e = *it;

        if (!e->shape() || !e->has_pose())
            continue;

        // Decompose entity pose into X Y YAW and Z ROLL PITCH
        geo::Pose3D pose_xya;
        geo::Pose3D pose_zrp;
        decomposePose(e->pose(), pose_xya, pose_zrp);

        const Shape2D* shape_2d;

        std::map<ed::UUID, Entity2DModel>::const_iterator it_model = models_.find(e->id());
        if (it_model == models_.end())
        {
            std::cout << "Going to project down " << e->id() << std::endl;

            Entity2DModel& entity_model = models_[e->id()];
            dml::project2D(e->shape()->getMesh().getTransformed(pose_zrp), entity_model.shape_2d);

            shape_2d = &entity_model.shape_2d;

            std::cout << "Done" << std::endl;
        }
        else
        {
            shape_2d = &(it_model->second.shape_2d);
        }

        geo::Transform2 pose_2d_SENSOR = sensor_pose_xya_2d.inverse() * XYYawToTransform2(pose_xya);

        beam_model_.RenderModel(*shape_2d, pose_2d_SENSOR, model_ranges);

        for(Shape2D::const_iterator it_contour = shape_2d->begin(); it_contour != shape_2d->end(); ++it_contour)
        {
            const std::vector<geo::Vec2>& model = *it_contour;
            for(unsigned int i = 0; i < model.size(); ++i)
            {
                unsigned int j = (i + 1) % model.size();
                const geo::Vec2& p1 = pose_2d_SENSOR * model[i];
                const geo::Vec2& p2 = pose_2d_SENSOR * model[j];

                cv::Point p1_canvas(p1.x * 100 + canvas.cols / 2, canvas.rows - p1.y * 100);
                cv::Point p2_canvas(p2.x * 100 + canvas.cols / 2, canvas.rows - p2.y * 100);

                cv::line(canvas, p1_canvas, p2_canvas, cv::Scalar(255, 0, 0), 2);
            }
        }

    }

    // -------------------------------------
    // Visualize


    for(unsigned int i = 0; i < ranges.size(); ++i)
    {
        geo::Vec2 p = beam_model_.CalculatePoint(i, ranges[i]);
        cv::Point p_canvas(p.x * 100 + canvas.cols / 2, canvas.rows - p.y * 100);
        cv::circle(canvas, p_canvas, 1, cv::Scalar(0, 255, 0));
    }

    for(unsigned int i = 0; i < model_ranges.size(); ++i)
    {
        geo::Vec2 p = beam_model_.CalculatePoint(i, model_ranges[i]);
        cv::Point p_canvas(p.x * 100 + canvas.cols / 2, canvas.rows - p.y * 100);
        cv::circle(canvas, p_canvas, 1, cv::Scalar(0, 0, 255));
    }

    cv::imshow("rgbd beams", canvas);
    cv::imshow("depth", depth / 10);
    cv::waitKey(3);
}

// ----------------------------------------------------------------------------------------------------

bool FitterPlugin::NextImage(const std::string& root_frame, rgbd::ImageConstPtr& image, geo::Pose3D& sensor_pose)
{
    // - - - - - - - - - - - - - - - - - -
    // Fetch kinect image and place in image buffer

    rgbd::ImageConstPtr rgbd_image = rgbd_client_.nextImage();
    if (rgbd_image && rgbd_image->getDepthImage().data)
        image_buffer_.push(rgbd_image);

    if (image_buffer_.empty())
        return false;

    rgbd_image = image_buffer_.front();

    // - - - - - - - - - - - - - - - - - -
    // Determine absolute kinect pose based on TF

    try
    {
        tf::StampedTransform t_sensor_pose;
        tf_listener_->lookupTransform("/map", rgbd_image->getFrameId(), ros::Time(rgbd_image->getTimestamp()), t_sensor_pose);
        geo::convert(t_sensor_pose, sensor_pose);
        image_buffer_.pop();
    }
    catch(tf::ExtrapolationException& ex)
    {
        try
        {
            // Now we have to check if the error was an interpolation or extrapolation error (i.e., the image is too old or
            // to new, respectively). If it is too old, discard it.

            tf::StampedTransform latest_sensor_pose;
            tf_listener_->lookupTransform(root_frame, rgbd_image->getFrameId(), ros::Time(0), latest_sensor_pose);
            // If image time stamp is older than latest transform, throw it out
            if ( latest_sensor_pose.stamp_ > ros::Time(rgbd_image->getTimestamp()) )
            {
                image_buffer_.pop();
                ROS_WARN_STREAM("[ED KINECT PLUGIN] Image too old to look-up tf: image timestamp = " << std::fixed
                                << ros::Time(rgbd_image->getTimestamp()));
            }

            return false;
        }
        catch(tf::TransformException& exc)
        {
            ROS_WARN("[ED KINECT PLUGIN] Could not get latest sensor pose (probably because tf is still initializing): %s", ex.what());
            return false;
        }
    }
    catch(tf::TransformException& ex)
    {
        ROS_WARN("[ED KINECT PLUGIN] Could not get sensor pose: %s", ex.what());
        return false;
    }

    // Convert from ROS coordinate frame to geolib coordinate frame
    sensor_pose.R = sensor_pose.R * geo::Matrix3(1, 0, 0, 0, -1, 0, 0, 0, -1);

    image = rgbd_image;

    return true;
}

// ----------------------------------------------------------------------------------------------------

ED_REGISTER_PLUGIN(FitterPlugin)