#include "planner.h"

/**
 * Control Modes:
 * 0: No PID
 * 1: Yaw PID with NO Thread
 * 2: Yaw PID with Controller Thread
 */
#define PID_MODE 0
#define SIMCTL
#define SIM_SEEDS
//#define DEBUG
#define SHOW_PATH
//#define FLEX

/**
 * Seed Files: 
 * seeds3.txt is valid but gives suboptimal results. Good Path. (6 - 8
 * seeds1.txt is for validation purposes ONLY. Grid A* - like path. Might be useful with DT. (101)
 * seeds.txt contains the full set of paths. Almost always stuck in inf loop.
 * seeds4.txt contains 5 seeds with arc-length ~75. Works fine. (4)
 * seeds5.txt contains 5 seeds with arc-lengths varying from 100 to 50. (2)
 * seeds2.txt contains 5 seeds 75 - 100 - 50 (16-20)
 */

#ifdef SIM_SEEDS
#define SEEDS_FILE "../src/Modules/Planner/seeds2.txt"
#else
#define SEEDS_FILE "../src/Modules/Planner/seeds1.txt"
#endif
#define OPEN 1
#define CLOSED 2
#define UNASSIGNED 3
#define VMAX 70
#define MAX_ITER 10000
#define MIN_RAD 70

using namespace std;

namespace planner_space {

    typedef struct state { // elemental data structure of openset
        Triplet pose;
        double g_dist, h_dist; // costs
        int seed_id;
        double g_obs, h_obs;
        int depth;
    } state;

    typedef struct seed_point {
        double x, y;
    } seed_point;

    typedef struct seed {
        Triplet dest;
        double cost;
        double k; // velocity ratio
        double vl, vr; // individual velocities
        vector<seed_point> seed_points;
    } seed;

    typedef struct open_map_element {
        char membership;
        double cost;
    } open_map_element;

    struct PoseCompare : public std::binary_function<Triplet, Triplet, bool> {

        bool operator() (Triplet const& triplet_1, Triplet const& triplet_2) const {
            double k11 = triplet_1.x;
            double k12 = triplet_1.y;
            double k13 = triplet_1.z;

            double cantor11 = 0.5 * (k11 + k12) * (k11 + k12 + 1) + k12;
            double cantor12 = 0.5 * (cantor11 + k13) * (cantor11 + k13 + 1) + k13;

            double k21 = triplet_2.x;
            double k22 = triplet_2.y;
            double k23 = triplet_2.z;

            double cantor21 = 0.5 * (k21 + k22) * (k21 + k22 + 1) + k22;
            double cantor22 = 0.5 * (cantor21 + k23) * (cantor21 + k23 + 1) + k23;

            return cantor12 < cantor22;
        }
    };

    struct StateCompare : public std::binary_function<state, state, bool> {

        bool operator() (state const& state_1, state const& state_2) const {
            double f1 = state_1.g_dist + state_1.h_dist;
            double f2 = state_2.g_dist + state_2.h_dist;

#ifdef DistTransform
            f1 += state_1.g_obs + state_1.h_obs;
            f2 += state_2.g_obs + state_2.h_obs;
#endif

            return f1 > f2;
        }
    };

    struct StateCompareDT : public std::binary_function<state, state, bool> {

        bool operator() (state const& state_1, state const& state_2) const {
            double f1 = state_1.g_dist + state_1.h_dist;
            double f2 = state_2.g_dist + state_2.h_dist;
            f1 += state_1.g_obs + state_1.h_obs;
            f2 += state_2.g_obs + state_2.h_obs;

            return f1 > f2;
        }
    };

    Triplet bot, target;
    vector<seed> seeds;
    Tserial *p;

    pthread_mutex_t controllerMutex;
    volatile double targetCurvature = 1;
    seed brake;
    seed leftZeroTurn, rightZeroTurn;
    /// ------------------------------------------------------------- ///

    void mouseHandler(int event, int x, int y, int flags, void* param) {
        if (event == CV_EVENT_LBUTTONDOWN) {
            //            i++;
            ROS_INFO("[PLANNER] Right mouse button pressed at: (%d, %d)", x, y);
        }
    }

    void *controllerThread(void *arg) {
        double myTargetCurvature;
        double myYaw = 0.5, previousYaw = 1, Kp = 5;
        int left_vel = 0, right_vel = 0;

        while (ros::ok()) {
            pthread_mutex_lock(&controllerMutex);
            myTargetCurvature = targetCurvature;
            pthread_mutex_unlock(&controllerMutex);

            previousYaw = myYaw;
            pthread_mutex_lock(&pose_mutex);
            myYaw = pose.orientation.z;
            pthread_mutex_unlock(&pose_mutex);

            left_vel = 40 + Kp * (myTargetCurvature - (myYaw - previousYaw) / 0.5);
            right_vel = 40 - Kp * (myTargetCurvature - (myYaw - previousYaw) / 0.5);

            ROS_INFO("[INFO] [Controller] %lf , %lf , %d , %d", myTargetCurvature, (myYaw - previousYaw)*2, left_vel, right_vel);

#ifndef SIMCTL

            p->sendChar('w');
            usleep(100);

            p->sendChar('0' + left_vel / 10);
            usleep(100);
            p->sendChar('0' + left_vel % 10);
            usleep(100);
            p->sendChar('0' + right_vel / 10);
            usleep(100);
            p->sendChar('0' + right_vel % 10);
            usleep(100);
#endif

            usleep(10000);
        }

        return NULL;
    }

    void loadSeeds() {
        int n_seeds;
        int return_status;
        double x, y, z;
        FILE *fp = fopen(SEEDS_FILE, "r");
        return_status = fscanf(fp, "%d\n", &n_seeds);
        if (return_status == 0) {
            ROS_ERROR("[PLANNER] Incorrect seed file format");
            Planner::finBot();
            exit(1);
        }

        for (int i = 0; i < n_seeds; i++) {
            seed s;

#ifdef SIM_SEEDS
            return_status = fscanf(fp, "%lf %lf %lf %lf %lf\n", &s.k, &x, &y, &z, &s.cost);
            if (return_status == 0) {
                ROS_ERROR("[PLANNER] Incorrect seed file format");
                Planner::finBot();
                exit(1);
            }

            s.vl = VMAX * s.k / (1 + s.k);
            s.vr = VMAX / (1 + s.k);
#else
            fscanf(fp, "%lf %lf %lf %lf %lf %lf\n", &s.vl, &s.vr, &x, &y, &z, &s.cost);
            s.k = s.vl / s.vr;
#endif

            //s.cost *= 1.2;
            s.dest.x = (int) x;
            s.dest.y = (int) y;
            s.dest.z = (int) z;

            int n_seed_points;
            return_status = fscanf(fp, "%d\n", &n_seed_points);
            if (return_status == 0) {
                ROS_ERROR("[PLANNER] Incorrect seed file format");
                Planner::finBot();
                exit(1);
            }

            for (int j = 0; j < n_seed_points; j++) {
                seed_point point;
                return_status = fscanf(fp, "%lf %lf\n", &point.x, &point.y);
                if (return_status == 0) {
                    ROS_ERROR("[PLANNER] Incorrect seed file format");
                    Planner::finBot();
                    exit(1);
                }

                s.seed_points.insert(s.seed_points.begin(), point);
            }
            seeds.insert(seeds.begin(), s);
        }
    }

    double distance(Triplet a, Triplet b) {
        return sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
    }

    bool isEqual(state a, state b) {
        double error = sqrt((a.pose.x - b.pose.x) * (a.pose.x - b.pose.x) +
                (a.pose.y - b.pose.y) * (a.pose.y - b.pose.y));

        return error < 35;
    }

    bool targetReached(state a, state b) {
        double error = sqrt((a.pose.x - b.pose.x) * (a.pose.x - b.pose.x) +
                (a.pose.y - b.pose.y) * (a.pose.y - b.pose.y));

        return error < 250;
    }
    
    void plotPoint(cv::Mat inputImgP, Triplet pose) {
        int x = pose.x;
        int y = MAP_MAX - pose.y - 1;
        int ax = x > MAP_MAX ? MAP_MAX - 1 : x;
        ax = x < 0 ? 0 : x;
        int ay = y > MAP_MAX ? MAP_MAX - 1 : y;
        ay = y < 0 ? 0 : y;

        int bx = ax;
        int by = y + 5 > MAP_MAX ? MAP_MAX - 1 : y + 5;
        by = y + 5 < 0 ? 0 : y + 5;

        srand(time(0));
        cv::line(
                inputImgP,
                cvPoint(ax, ay),
                cvPoint(bx, by),
                CV_RGB(127, 127, 127),
                2,
                CV_AA,
                0);
    }

    void startThread(pthread_t *thread_id, pthread_attr_t *thread_attr, void *(*thread_name) (void *)) {
        if (pthread_create(thread_id, thread_attr, thread_name, NULL)) {
            cout << "[PLANNER] [ERROR] Unable to create thread" << endl;
            pthread_attr_destroy(thread_attr);
            exit(1);
        }
        sleep(1);
    }

    void initBot() {
        if (PID_MODE == 2) {
            pthread_attr_t attr;
            pthread_t controller_id;

            pthread_mutex_init(&controllerMutex, NULL);
            pthread_mutex_trylock(&controllerMutex);
            pthread_mutex_unlock(&controllerMutex);

            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

            startThread(&controller_id, &attr, &controllerThread);

            pthread_attr_destroy(&attr);
        }

#ifndef SIMCTL
        p = new Tserial();
        p->connect(BOT_COM_PORT, BOT_BAUD_RATE, spNONE);
        usleep(100);
#endif
    }

    geometry_msgs::Twist sendCommand(seed s) {
        geometry_msgs::Twist cmdvel;

        int left_vel = 0;
        int right_vel = 0;
        float left_velocity = s.vl;
        float right_velocity = s.vr;

        if ((left_velocity == 0) && (right_velocity == 0)) {
            return cmdvel;
        } else if ((left_velocity >= 0) && (right_velocity >= 0)) {
            switch (PID_MODE) {
                case 0:
                {
                    if (left_velocity == right_velocity) {
                        double vavg = 80;
                        left_vel = right_vel = vavg;
			printf("straight seed\n");
                    } else if (s.k == 1.258574 || s.k == 0.794550) {
                        double vavg = 50;
                        double aggression = 1;
                        s.k = s.k < 1 ? s.k / aggression : s.k * aggression;
                        left_vel = (int) 2 * vavg * s.k / (1 + s.k);
                        right_vel = (int) (2 * vavg - left_vel);
			printf("soft seed\n");
                    } else if (s.k == 1.352941 || s.k == 0.739130) {
                        double vavg = 20;
                        double aggression = 1.5;
                        s.k = s.k < 1 ? s.k / aggression : s.k * aggression;
                        left_vel = (int) 2 * vavg * s.k / (1 + s.k);
                        right_vel = (int) (2 * vavg - left_vel);
			printf("hard seed\n");
                    }

                    break;
                }
                case 1:
                {
                    double myTargetCurvature = 5.0 * ((double) (left_velocity - right_velocity)) / (left_velocity + right_velocity);
                    static double myYaw = 0.5;
                    static double previousYaw = 1;
                    static double errorSum = 0;
                    static double previousError;
                    double Kp = 6.4, Kd = 0.01, Ki = 0.0001;
                    left_vel = 0;
                    right_vel = 0;
                    int mode = 4;

                    double error = (myTargetCurvature - (myYaw - previousYaw) / 0.37);
                    errorSum += error;

                    if (s.k > 1.35) {
                        mode = 2;
                    } else if (s.k > 1.25) {
                        mode = 3;
                    } else if (s.k > 0.99) {
                        mode = 4;
                    } else if (s.k > 0.79) {
                        mode = 5;
                    } else if (s.k > 0.73) {
                        mode = 6;
                    } else {
                        mode = 4;
                    }

                    int hashSpeedLeft[9] = {23, 30, 35, 38, 40, 42, 45, 50, 57};
                    int hashSpeedRight[9] = {57, 50, 45, 42, 40, 38, 35, 30, 23};

                    previousYaw = myYaw;

                    pthread_mutex_lock(&pose_mutex);
                    myYaw = pose.orientation.z;
                    pthread_mutex_unlock(&pose_mutex);

                    mode += (int) (Kp * error + Ki * errorSum + Kd * (error - previousError));

                    if (mode < 0) {
                        mode = 0;
                    }
                    if (mode > 8) {
                        mode = 8;
                    }

                    left_vel = hashSpeedLeft[mode];
                    right_vel = hashSpeedRight[mode];

                    ROS_INFO("[PID] %lf, %lf, %lf, %lf, %d, %d",
                            myTargetCurvature,
                            (myYaw - previousYaw) / 0.37,
                            left_velocity,
                            right_velocity,
                            left_vel,
                            right_vel);

                    break;
                }
                case 2:
                {
                    pthread_mutex_lock(&controllerMutex);
                    targetCurvature = 5.0 * ((double) (s.k - 1.0)) / (s.k + 1.0);
                    ROS_INFO("Updated : %lf, k = %lf, left = %lf, right = %lf", targetCurvature, s.k, s.vl, s.vr);
                    pthread_mutex_unlock(&controllerMutex);

                    break;
                }
            }
        } else {
            ROS_INFO("REVERSING");
            
            left_vel = left_velocity;
            right_vel = right_velocity;
        }

#ifndef SIMCTL
        char arr[] = {'w',
            '0' + left_vel / 10,
            '0' + left_vel % 10,
            '0' + right_vel / 10,
            '0' + right_vel % 10, '/0'};

        p->sendArray(arr, 5);
        usleep(100);
#endif  

        left_vel = left_vel > 80 ? 80 : left_vel;
        right_vel = right_vel > 80 ? 80 : right_vel;
        left_vel = left_vel < -80 ? -80 : left_vel;
        right_vel = right_vel < -80 ? -80 : right_vel;

        //	if(s.vl==-30&&s.vr==30)
        //	{
        //	left_vel=-30;
        //	right_vel=30;
        //	}
        //	if(s.vl==30&&s.vr==-30)
        //	{
        //	left_vel=30;
        //	right_vel=-30;
        //	}

        double scale = 100;
        double w = 0.55000000;
        cmdvel.linear.x = (left_vel + right_vel) / (2 * scale);
        cmdvel.linear.y = 0;
        cmdvel.linear.z = 0;
        cmdvel.angular.x = 0;
        cmdvel.angular.y = 0;
        cmdvel.angular.z = (left_vel - right_vel) / (w * scale);

//        std::cout << "linear: " << cmdvel.linear.x << " angular: " << cmdvel.angular.z << std::endl;
        ROS_INFO("[Planner] Command : (%d, %d)", left_vel, right_vel);
        return cmdvel;
    }

    geometry_msgs::Twist reconstructPath(map<Triplet, state, PoseCompare> came_from, state current, cv::Mat inputImgP) {
        pthread_mutex_lock(&path_mutex);

        geometry_msgs::Twist cmdvel;

        path.clear();

        int seed_id = -1;
        state s = current;
        while (came_from.find(s.pose) != came_from.end()) {
#if defined SHOW_PATH || defined DEBUG
            plotPoint(inputImgP, s.pose);
#endif

            path.insert(path.begin(), s.pose);
            seed_id = s.seed_id;
            s = came_from[s.pose];
        }

        pthread_mutex_unlock(&path_mutex);

#if defined(DEBUG)
        cv::imshow("[PLANNER] Map", inputImgP);
        cvWaitKey(0);
#endif

#ifdef FLEX
        int path_length = path.size() > 5 ? 5 : path.size();
        double path_slope = 0;
        for (int i = 0; i < path_length - 1; i++) {
            double point_slope;
            double denom = path[i + 1].x - path[i].x;
            denom = denom == 0 ? 0.0000001 : denom;
            point_slope = (path[i + 1].y - path[i].y) / (denom);
            path_slope += point_slope;
        }
        path_slope /= path_length;

        seed final_seed;
        final_seed.vl = final_seed.vr = 10;

        double radius_of_curvature;
        if (path_slope > 0) {
            // LEFT
            radius_of_curvature = MIN_RAD + path_slope;
            final_seed.k = (2 * radius_of_curvature + MIN_RAD) / (2 * radius_of_curvature - MIN_RAD);
        } else {
            // RIGHT
            radius_of_curvature = MIN_RAD - path_slope;
            final_seed.k = (2 * radius_of_curvature - MIN_RAD) / (2 * radius_of_curvature + MIN_RAD);
        }

        cmdvel = sendCommand(final_seed);
#else
        if (seed_id != -1) {
            cmdvel = sendCommand(seeds[seed_id]);
        } else {
            ROS_ERROR("[PLANNER] Invalid Command Requested");
            Planner::finBot();
        }
#endif

        return cmdvel;
    }

    void reconstructPath(map<Triplet, state, PoseCompare> came_from, cv::Mat inputImgP, state current) {
        pthread_mutex_lock(&path_mutex);

        path.clear();

        int seed_id = -1;
        state s = current;
        while (came_from.find(s.pose) != came_from.end()) {
            plotPoint(inputImgP, s.pose);
            path.insert(path.begin(), s.pose);
            seed_id = s.seed_id;
            s = came_from[s.pose];
        }

        cv::imshow("[PLANNER] Map", inputImgP);
        cvWaitKey(WAIT_TIME);

        pthread_mutex_unlock(&path_mutex);

        if (seed_id != -1) {
            sendCommand(seeds[seed_id]);
        } else {
            ROS_ERROR("[PLANNER] Invalid Command Requested");
            Planner::finBot();
        }
    }

    vector<state> neighborNodes(state current) {
        vector<state> neighbours;
        for (unsigned int i = 0; i < seeds.size(); i++) {
            state neighbour;
            double sx = seeds[i].dest.x;
            double sy = seeds[i].dest.y;
            double sz = seeds[i].dest.z;

            neighbour.pose.x = (int) (current.pose.x +
                    sx * sin(current.pose.z * (CV_PI / 180)) +
                    sy * cos(current.pose.z * (CV_PI / 180)));
            neighbour.pose.y = (int) (current.pose.y +
                    -sx * cos(current.pose.z * (CV_PI / 180)) +
                    sy * sin(current.pose.z * (CV_PI / 180)));

            neighbour.pose.z = (int) (sz - (90 - current.pose.z));
            neighbour.h_dist = 0;
            neighbour.seed_id = i;
            neighbour.g_dist = seeds[i].cost;
            neighbour.h_obs = 0;
            neighbour.g_obs = 0;

            neighbours.push_back(neighbour);
        }

        return neighbours;
    }

    bool onTarget(state current, state goal) {
        for (unsigned int i = 0; i < seeds.size(); i++) {
            for (unsigned int j = 0; j < seeds[i].seed_points.size(); j++) {
                state temp;
                double sx = seeds[i].seed_points[j].x;
                double sy = seeds[i].dest.y;
                double sz = seeds[i].dest.z;

                temp.pose.x = (int) (current.pose.x +
                        sx * sin(current.pose.z * (CV_PI / 180)) +
                        sy * cos(current.pose.z * (CV_PI / 180)));
                temp.pose.y = (int) (current.pose.y +
                        -sx * cos(current.pose.z * (CV_PI / 180)) +
                        sy * sin(current.pose.z * (CV_PI / 180)));

                if (isEqual(temp, goal)) {
                    return true;
                }
            }
        }
        return false;
    }

    void print(state s) {
        double f = s.g_dist + s.h_dist;
        cout << "{ " <<
                s.pose.x << " , " <<
                s.pose.y << " , " <<
                s.pose.z << " , " <<
                f << " }" << endl;
    }

    bool isWalkable(state parent, state s) {
        int flag = 1;

        for (unsigned int i = 0; i < seeds[s.seed_id].seed_points.size(); i++) {
            int x, y;
            double alpha = parent.pose.z;

            int tx, ty;
            tx = seeds[s.seed_id].seed_points[i].x;
            ty = seeds[s.seed_id].seed_points[i].y;

            x = (int) (tx * sin(alpha * (CV_PI / 180)) + ty * cos(alpha * (CV_PI / 180)) + parent.pose.x);
            y = (int) (-tx * cos(alpha * (CV_PI / 180)) + ty * sin(alpha * (CV_PI / 180)) + parent.pose.y);

            if (((0 <= x) && (x < MAP_MAX)) && ((0 <= y) && (y < MAP_MAX))) {
                local_map[x][y] == 0 ? flag *= 1 : flag *= 0;
            } else {
                return false;
            }
        }

        return flag == 1;
    }

    void closePlanner() {

    }

    void addObstacleP(cv::Mat inputImgP, int x, int y, int r) {
        for (int i = -r; i < r; i++) {
            if (x + i >= 0 && x + i <= MAP_MAX) {
                for (int j = -r; j < r; j++) {
                    if (y + j >= 0 && y + j <= MAP_MAX) {
                        local_map[x + i][y + j] = 255;
                    }
                }
            }
        }

#if defined(DEBUG) || defined(SHOW_PATH)
        cv::circle(inputImgP, cvPoint(x, MAP_MAX - y - 1), r, CV_RGB(255, 255, 255), -1, CV_AA, 0);
#endif
    }
}
