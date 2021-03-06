#include <opencv2/opencv.hpp>

#include "myslam/algorithm.h"
#include "myslam/backend.h"
#include "myslam/feature.h"
#include "myslam/frontend.h"
#include "myslam/g2o_types.h"
#include "myslam/map.h"
#include "myslam/viewer.h"

namespace myslam {

    Frontend::Frontend() {

        // goodFeaturesToTrack
        gftt_ = cv::GFTTDetector::create(num_features_, 0.01, 20);

    }

    bool Frontend::AddFrame(Frame::Ptr frame) {

        current_frame_ = frame;

        switch (status_) {
            case FrontendStatus::INITING:
                StereoInit();
                break;

            case FrontendStatus::TRACKING_GOOD:
            case FrontendStatus::TRACKING_BAD:
                Track();
                break;
            case FrontendStatus::LOST:
                Reset();
                break;
        }
        // std::cout << "Adding frame, done!" << std::endl;

        last_frame_ = current_frame_;
        return true; // status: TRACKING_GOOD
    }

    bool Frontend::Track() {

        if (last_frame_) {
            /**
             * set the pose of current frame
             * initialize pose of current frame before optimization
             * current_frame_->pose_ is set
             */
            current_frame_->SetPose(relative_motion_ * last_frame_->Pose());
        }

        int num_track_last = TrackLastFrame();

        tracking_inliers_ = EstimateCurrentPose();

        // [20, 50]
        if (tracking_inliers_ > num_features_tracking_) {
            // 50 < tracking_inliers_
            // tracking good
            status_ = FrontendStatus::TRACKING_GOOD;
        } else if (tracking_inliers_ > num_features_tracking_bad_) {
            // 20 < tracking_inliers_ <= 50
            // tracking bad
            status_ = FrontendStatus::TRACKING_BAD;
        } else {
            // tracking_inliers_ <= 20
            // lost
            status_ = FrontendStatus::LOST;
        }

        InsertKeyframe();

        // calculate the relative motion, inverse() is important
        relative_motion_ = current_frame_->Pose() * last_frame_->Pose().inverse();

        if (viewer_) viewer_->AddCurrentFrame(current_frame_);

        return true;
    }

    bool Frontend::InsertKeyframe() {

        if (tracking_inliers_ >= num_features_needed_for_keyframe_) {
            // still have enough features, don't insert keyframe
            return false;
        }

        /**
         * if the tracking_inliers_ is relative small
         * current frame will be set to the keyframe,
         * then insert the current frame into keyframes group
         */

        current_frame_->SetKeyFrame();
        map_->InsertKeyFrame(current_frame_);
        LOG(INFO) << "Set frame " << current_frame_->id_ << " as keyframe "
                    << current_frame_->keyframe_id_;

        // step 1: detect and extract new features
        SetObservationsForKeyFrame();
        DetectFeatures();

        // step 2.1: find the corresponding features in right image
        FindFeaturesInRight();
        // step 2.2: triangulate map points, and compute new landmarks
        TriangulateNewPoints();

        // step 3: add the new keyframe and landmarks into the map,
        //         and activate a backend optimization process
        backend_->UpdateMap();

        if (viewer_) viewer_->UpdateMap();

        return true;
    }

    void Frontend::SetObservationsForKeyFrame() {

        for (auto &feat : current_frame_->features_left_) {

            auto mp = feat->map_point_.lock();

            // the 2D features corresponding to the 3D landmark
            if (mp) mp->AddObservation(feat);
        }

    }

    int Frontend::TriangulateNewPoints() {

        std::vector<SE3> poses{camera_left_->pose(), camera_right_->pose()};

        SE3 current_pose_Twc = current_frame_->Pose().inverse();

        int cnt_triangulated_pts = 0;

        for (size_t i = 0; i < current_frame_->features_left_.size(); ++i) {
            /**
             * For each feature in current frame,
             * if there is no associated 3D MapPoint/landmark
             * corresponding to the feature in the left image,
             * and there is a corresponding feature in right image,
             * then, use triangulation to estimate the MapPoint/landmark,
             * and finally, insert the new MapPoint/landmark into the existed map
             */
            if (current_frame_->features_left_[i]->map_point_.expired() &&
                current_frame_->features_right_[i] != nullptr) {

                std::vector<Vec3> points{
                    camera_left_->pixel2camera( // feature is represented by camera coordinate
                            Vec2(current_frame_->features_left_[i]->position_.pt.x,
                                    current_frame_->features_left_[i]->position_.pt.y)),
                    camera_right_->pixel2camera( // feature is represented by camera coordinate
                            Vec2(current_frame_->features_right_[i]->position_.pt.x,
                                 current_frame_->features_right_[i]->position_.pt.y))
                };

                // 3D point in the camera coordinate
                Vec3 pworld = Vec3::Zero();

                if (triangulation(poses, points, pworld) && pworld[2] > 0) {
                    // if triangulation is successful, and pworld is the new calculated MapPoint/landmark
                    auto new_map_point = MapPoint::CreateNewMappoint();
                    pworld = current_pose_Twc * pworld; // in the world coordinate
                    new_map_point->SetPos(pworld);

                    // link the new 3D MapPoint/landmark and features of left and right current frames
                    new_map_point->AddObservation(current_frame_->features_left_[i]);
                    new_map_point->AddObservation(current_frame_->features_right_[i]);
                    current_frame_->features_left_[i]->map_point_ = new_map_point;
                    current_frame_->features_right_[i]->map_point_ = new_map_point;

                    // insert the new 3D MapPoint/landmark into the existed map
                    map_->InsertMapPoint(new_map_point);

                    cnt_triangulated_pts++;
                }
            }
        }

        LOG(INFO) << "There are " << cnt_triangulated_pts << " new landmarks inserted into the existed 3D map.";

        return cnt_triangulated_pts;
    }

    int Frontend::EstimateCurrentPose() {
        // setup g2o
        typedef g2o::BlockSolver_6_3 BlockSolverType;
        typedef g2o::LinearSolverDense<BlockSolverType::PoseMatrixType> LinearSolverType;
        auto solver = new g2o::OptimizationAlgorithmLevenberg(
                g2o::make_unique<BlockSolverType>(g2o::make_unique<LinearSolverType>()));
        g2o::SparseOptimizer optimizer;
        optimizer.setAlgorithm(solver);

        // add vertex
        // set current camera pose as vertex
        VertexPose *vertex_pose = new VertexPose(); // camera vertex_pose
        vertex_pose->setId(0);
        vertex_pose->setEstimate(current_frame_->Pose()); // Pose is to be estimated, x
        optimizer.addVertex(vertex_pose);

        // K
        Mat33 K = camera_left_->K();

        // add edges
        // set projected point in the current frame as edges
        // there are many points, size = current_frame_->features_left_.size()
        int index = 1;
        std::vector<EdgeProjectionPoseOnly *> edges;
        std::vector<Feature::Ptr> features;

        // many 2D features correspond to many MapPoints/landmarks
        // each edge corresponds to a MapPoint-2D features, id is index (index++)
        // all these edges correspond to only one camera pose
        for (size_t i = 0; i < current_frame_->features_left_.size(); ++i) {
            auto mp = current_frame_->features_left_[i]->map_point_.lock();
            if (mp) {
                features.push_back(current_frame_->features_left_[i]);
                EdgeProjectionPoseOnly* edge = new EdgeProjectionPoseOnly(mp->pos_, K);
                edge->setId(index);

                // set the connected vertex
                edge->setVertex(0, vertex_pose); // only for camera pose

                // set measurement/practical value, z
                edge->setMeasurement(toVec2(current_frame_->features_left_[i]->position_.pt));

                // information matrix
                edge->setInformation(Eigen::Matrix2d::Identity());

                edge->setRobustKernel(new g2o::RobustKernelHuber);
                edges.push_back(edge);
                optimizer.addEdge(edge);
                index++;
            }
        }

        // determine the outliers
        const double chi2_th = 5.991;
        int cnt_outlier = 0;
        for (int iteration = 0; iteration < 4; ++iteration) {
            vertex_pose->setEstimate(current_frame_->Pose());
            optimizer.initializeOptimization();
            optimizer.optimize(10);

            cnt_outlier = 0;

            // count the outliers
            for (size_t i = 0; i < edges.size(); ++i) {
                auto e = edges[i];
                // features->is_outlier = false (default)
                if (features[i]->is_outlier_) {
                    e->computeError();
                }
                if (e->chi2() > chi2_th) { // outlier
                    features[i]->is_outlier_ = true; // default: false
                    e->setLevel(1);
                    cnt_outlier++;
                } else { // e->chi2() <= chi2_th
                    features[i]->is_outlier_ = false;
                    e->setLevel(0);
                }
                if (iteration == 2) {
                    e->setRobustKernel(nullptr);
                }
            }
        }

        LOG(INFO) << "Outlier/Inlier in pose estimating: "
                  << cnt_outlier << "/" << features.size() - cnt_outlier;

        // Set pose and outlier
        current_frame_->SetPose(vertex_pose->estimate());

        LOG(INFO) << "Current Pose = \n" << current_frame_->Pose().matrix();

        for (auto &feat : features) {
            if (feat->is_outlier_) { // true
                feat->map_point_.reset();
                feat->is_outlier_ = false; // maybe we can still use it in future
            }
        }

        return features.size() - cnt_outlier;
    }

    int Frontend::TrackLastFrame() {
        // use LK flow to estimate 2D features in the right frame
        std::vector<cv::Point2f> kps_last, kps_current;
        for (auto &kp : last_frame_->features_left_) {
            if (kp->map_point_.lock()) {
                /**
                 * public member function
                 * std::weak_ptr::lock
                 * shared_ptr<element_type> lock() const noexcept;
                 * Lock and restore weak_ptr
                 * Returns a shared_ptr with the information preserved by the weak_ptr object if it is not expired.
                 *
                 * If the weak_ptr object has expired (including if it is empty),
                 * the function returns an empty shared_ptr (as if default-constructed).
                 * Because shared_ptr objects count as an owner, this function locks the owned pointer,
                 * preventing it from being released (for at least as long as the returned object does not releases it).
                 *
                 * This operation is executed atomically.
                 *
                 * Code:
                 * std::shared_ptr<int> sp1,sp2;
                 * std::weak_ptr<int> wp;
                 *                                      // sharing group:
                 *                                      // --------------
                 * sp1 = std::make_shared<int> (20);    // sp1
                 * wp = sp1;                            // sp1, wp
                 *
                 * sp2 = wp.lock();                     // sp1, wp, sp2
                 * sp1.reset();                         //      wp, sp2
                 *
                 * sp1 = wp.lock();
                 *
                 * Output:
                 * *sp1: 20
                 * *sp2: 20
                 */
                // use project point
                auto mp = kp->map_point_.lock();
                auto px = camera_left_->world2pixel(mp->pos_, current_frame_->Pose());
                kps_last.push_back(kp->position_.pt);
                kps_current.push_back(cv::Point2f(px[0], px[1]));
            } else {
                kps_last.push_back(kp->position_.pt);
                kps_current.push_back(kp->position_.pt);
            }
        }

        std::vector<uchar> status;
        cv::Mat error;
        cv::calcOpticalFlowPyrLK(
                last_frame_->left_img_, current_frame_->left_img_, kps_last,
                kps_current, status, error, cv::Size(11, 11), 3,
                cv::TermCriteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 30, 0.01),
                cv::OPTFLOW_USE_INITIAL_FLOW);

        int num_good_pts = 0;

        for (size_t i = 0; i < status.size(); ++i) {
            if (status[i]) {
                cv::KeyPoint kp(kps_current[i], 7);
                /**
                 * template class cv::Point_
                 * Template class for 2D points specified by its coordinates x and y
                 *    Point_(_Tp _x, _Tp _y);
                 *
                 * typedef Point_<float> Point2f;
                 *
                 * class cv::KeyPoint
                 * Data structure for salient point detectors.
                 *    KeyPoint(
                 *    Point2f _pt,
                 *    float _size,
                 *    float _angle = -1,
                 *    float _response = 0,
                 *    int _octave = 0,
                 *    int _class_id = -1
                 *    );
                 */
                Feature::Ptr feature(new Feature(current_frame_, kp));
                feature->map_point_ = last_frame_->features_left_[i]->map_point_;
                current_frame_->features_left_.push_back(feature);
                num_good_pts++;
            }
        }

        LOG(INFO) << "Find " << num_good_pts << " in the last image.";
        return num_good_pts;
    }

    bool Frontend::StereoInit() {
        int num_features_left = DetectFeatures();
        int num_coor_features = FindFeaturesInRight();
        if (num_coor_features < num_features_init_)
            return false;

        bool build_map_success = BuildInitMap();
        if (build_map_success) {
            status_ = FrontendStatus::TRACKING_GOOD;
            if (viewer_) {
                viewer_->AddCurrentFrame(current_frame_);
                viewer_->UpdateMap();
            }
            return true;
        }
        return false;
    }

    int Frontend::DetectFeatures() {
        cv::Mat mask(current_frame_->left_img_.size(), CV_8UC1, 255);
        for (auto &feat : current_frame_->features_left_) {
            cv::rectangle(mask, feat->position_.pt - cv::Point2f(10, 10),
                    feat->position_.pt + cv::Point2f(10, 10), 0, cv::FILLED);
        }

        std::vector<cv::KeyPoint> keypoints;
        gftt_->detect(current_frame_->left_img_, keypoints, mask);
        int cnt_detected = 0;
        for (auto &kp : keypoints) {
            current_frame_->features_left_.push_back(
                    Feature::Ptr(new Feature(current_frame_, kp)));
            cnt_detected++;
        }

        LOG(INFO) << "Detect " << cnt_detected << " new features.";
        return cnt_detected;
    }

    int Frontend::FindFeaturesInRight() {
        // use LK flow to estimate points in the right image
        std::vector<cv::Point2f> kps_left, kps_right;
        for (auto &kp : current_frame_->features_left_) {
            kps_left.push_back(kp->position_.pt);
            auto mp = kp->map_point_.lock();
            if (mp) {
                // use projected points as initial value
                auto px = camera_right_->world2pixel(mp->pos_, current_frame_->Pose());
                kps_right.push_back(cv::Point2f(px[0], px[1]));
            } else {
                // use the pixel as same as the left image
                kps_right.push_back(kp->position_.pt);
            }
        }

        std::vector<uchar> status;
        cv::Mat error;
        // return status, error
        cv::calcOpticalFlowPyrLK(
                current_frame_->left_img_, current_frame_->right_img_, kps_left,
                kps_right, status, error, cv::Size(11, 11), 3,
                cv::TermCriteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS,
                        30, 0.01), cv::OPTFLOW_USE_INITIAL_FLOW);

        int num_good_pts = 0;
        for (size_t i = 0; i < status.size(); ++i) {
            if (status[i]) {
                cv::KeyPoint kp(kps_right[i], 7);
                Feature::Ptr feat(new Feature(current_frame_, kp));
                feat->is_on_left_image_ = false;
                current_frame_->features_right_.push_back(feat);
                num_good_pts++;
            } else {
                current_frame_->features_right_.push_back(nullptr);
            }
        }
        LOG(INFO) << "Find " << num_good_pts << " in the right image.";
        return num_good_pts;
    }

    bool Frontend::BuildInitMap() {
        std::vector<SE3> poses{camera_left_->pose(), camera_right_->pose()};
        size_t cnt_init_landmarks = 0;
        for (size_t i = 0; i < current_frame_->features_left_.size(); ++i) {
            if (current_frame_->features_right_[i] == nullptr) continue;
            // create map point from triangulation
            std::vector<Vec3> points{
                camera_left_->pixel2camera(
                        Vec2(current_frame_->features_left_[i]->position_.pt.x,
                                current_frame_->features_left_[i]->position_.pt.y)),
                camera_right_->pixel2camera(
                        Vec2(current_frame_->features_right_[i]->position_.pt.x,
                             current_frame_->features_right_[i]->position_.pt.y))
            };
            Vec3 pworld = Vec3 ::Zero();

            if (triangulation(poses, points, pworld) && pworld[2] > 0) {
                auto new_map_point = MapPoint::CreateNewMappoint();
                new_map_point->SetPos(pworld);
                new_map_point->AddObservation(current_frame_->features_left_[i]);
                new_map_point->AddObservation(current_frame_->features_right_[i]);
                current_frame_->features_left_[i]->map_point_ = new_map_point;
                current_frame_->features_right_[i]->map_point_ = new_map_point;
                cnt_init_landmarks++;
                map_->InsertMapPoint(new_map_point);
            }
        }
        current_frame_->SetKeyFrame();
        map_->InsertKeyFrame(current_frame_);
        backend_->UpdateMap();

        LOG(INFO) << "Initial map created with " << cnt_init_landmarks << " map points";

        return true;
    }

    bool Frontend::Reset() {
        LOG(INFO) << "Reset is not implemented. ";
        return true;
    }

} // namespace


