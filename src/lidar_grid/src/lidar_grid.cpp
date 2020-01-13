#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>
#include <nav_msgs/GridCells.h>
#include <std_msgs/Float32.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/ModelCoefficients.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/common/common.h>
#include <cmath>
#include <time.h>
#include <vector>
#include "raw_data/Ultrasonic.h"

class Grid
{
public:
    Grid(void) : grid_cloud_ptr(new pcl::PointCloud<pcl::PointXYZ>), IsDrivable(false), IsChecked(false) { }
    ~Grid() { }
    pcl::PointCloud<pcl::PointXYZ>::Ptr grid_cloud_ptr;
    bool IsDrivable;
    bool IsChecked;
};

///////////////////////激光雷达点云栅格化处理类 RANSAC方法////////////////////////
class LidarCloudHandler
{
protected:
    ros::NodeHandle nh;
    ros::Subscriber pc_sub;
    ros::Subscriber left_ultrasonic_sub;
    ros::Subscriber right_ultrasonic_sub;
    ros::Publisher grid_pub;
    ros::Publisher time_pub;
    ros::Publisher ground_z_pub;
    std::string fixed_frame;
    float left_ultrasonic[4];
    float right_ultrasonic[4];
    int R;                    //半径上的分割数
    int TH;                   //角度上的分割数
    float grid_size_r;        //半径上的分辨率
    float grid_size_th;       //角度上的分辨率
    float radius;             //半径
    float grid_size;          //输出栅格大小
    float threshold;         //地面点阈值
    float cut_width;          //可通行域裁剪宽度
    int y_width;              //输出栅格地图一侧宽度
    int x_forward;            //输出栅格地图前向长度
    int x_backward;           //输出栅格地图后向长度
    float RANSAC_threshold;   //RANSAC阈值

public:
    LidarCloudHandler(void)
    {
        pc_sub = nh.subscribe("cali_pc", 1, &LidarCloudHandler::rasterization, this);    //接收话题：cali_pc
        left_ultrasonic_sub  = nh.subscribe("left_ultrasonic", 10, &LidarCloudHandler::left_ultrasonic_cb, this);   //接收话题：left_ultrasonic
        right_ultrasonic_sub = nh.subscribe("right_ultrasonic", 10, &LidarCloudHandler::right_ultrasonic_cb, this); //接收话题：right_ultrasonic

        grid_pub = nh.advertise<nav_msgs::GridCells>("grid_cell", 1);                    //发布话题：grid_cell
        time_pub = nh.advertise<std_msgs::Float32>("time", 1);                           //发布话题：time
        ground_z_pub = nh.advertise<std_msgs::Float32>("ground_z", 1);                   //发布话题：ground_z

        nh.param<std::string>("/lidar_grid/fixed_frame", fixed_frame, "velodyne");
        nh.param<int>("/lidar_grid/R", R, 60);
        nh.param<int>("/lidar_grid/TH", TH, 180);
        nh.param<float>("/lidar_grid/grid_size_r", grid_size_r, 0.4);
        nh.param<float>("/lidar_grid/grid_size", grid_size, 0.2);
        nh.param<float>("/lidar_grid/threshold", threshold, 0.15);
        nh.param<float>("/lidar_grid/cut_width", cut_width, 1.7);
        nh.param<int>("/lidar_grid/y_width", y_width, 50);
        nh.param<int>("/lidar_grid/x_forward", x_forward, 100);
        nh.param<int>("/lidar_grid/x_backward", x_backward, 0);
        nh.param<float>("/lidar_grid/RANSAC_threshold", RANSAC_threshold, 0.2);
        grid_size_th = 2*M_PI/TH;
        radius = R*grid_size_r;
    }
    ~LidarCloudHandler() { }
    void rasterization(const sensor_msgs::PointCloud2ConstPtr& input);
    void left_ultrasonic_cb(const raw_data::Ultrasonic& input);
    void right_ultrasonic_cb(const raw_data::Ultrasonic& input);
};

void LidarCloudHandler::left_ultrasonic_cb(const raw_data::Ultrasonic& input){
    for(int i=0; i<4; ++i){
        left_ultrasonic[i] = input.probe[i];
    }
}

void LidarCloudHandler::right_ultrasonic_cb(const raw_data::Ultrasonic& input){
    for(int i=0; i<4; ++i){
        right_ultrasonic[i] = input.probe[i];
    }
}

//////////////////////////激光雷达点云栅格化及地面可通行区域提取函数///////////////////////////
void LidarCloudHandler::rasterization(const sensor_msgs::PointCloud2ConstPtr& input)
{
    clock_t start = clock();
    nav_msgs::GridCells grid_cell;
    grid_cell.header.frame_id = fixed_frame;  //rviz中的fixed frame
    grid_cell.cell_height = grid_size;        //栅格大小
    grid_cell.cell_width = grid_size;
    geometry_msgs::Point free_road;

    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_raw_ptr  (new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_temp_ptr (new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_temp2_ptr (new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_ground_ptr (new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_obstacle_ptr (new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_boundary_ptr  (new pcl::PointCloud<pcl::PointXYZ>);
    pcl::fromROSMsg(*input, *cloud_raw_ptr);

    Grid grid[R][TH];      //建立极坐标栅格地图grid
    float r = 0, th = 0;
    int a = 0, t = 0;

//////////////////////////////遍历输入点云，初步筛选，将点划入极坐标栅格内///////////////////////////////
    for (int m=0; m<cloud_raw_ptr->size(); ++m)   
    {
        r = sqrt(cloud_raw_ptr->points[m].x * cloud_raw_ptr->points[m].x
                 + cloud_raw_ptr->points[m].y * cloud_raw_ptr->points[m].y);  //XY平面内，点到原点的距离

        th = acos(cloud_raw_ptr->points[m].x/r);//点对应的向量的角度（以x轴正方向为零角度）
    
        if(cloud_raw_ptr->points[m].y<0)
        {
            th = 2*M_PI-th;   //角度范围 车周0~TH度
        }                  

        t = (int)floor(th/grid_size_th) % TH;   //坐标转换为极坐标栅格地图坐标
        a = floor(r/grid_size_r);
        a = a >= R? R-1 : a;
        grid[a][t].grid_cloud_ptr->points.push_back(cloud_raw_ptr->points[m]);  //把该点划入对应栅格内
    }

/////////////////////////////////初步筛选出地面点云和障碍物点云/////////////////////////////
    pcl::PointXYZ minpoint, maxpoint;
    float h = 0;
    
    for (int i=0; i<R; ++i)      //遍历栅格地图grid
    {
        for(int j=0; j<TH; ++j)
        {
            if(grid[i][j].grid_cloud_ptr->size()>0)    //如果某栅格内有数据点
            {
                pcl::getMinMax3D (*grid[i][j].grid_cloud_ptr, minpoint, maxpoint);   //这里的最大最小是值，不是对应一个点
                h = maxpoint.z - minpoint.z;                                         //该栅格内数据点的最大高度差
                
                if (h<threshold)  //最大高度差小于阈值的点归为地面候选点
                {
                    for(int m=0; m<grid[i][j].grid_cloud_ptr->size(); ++m)
                    {
                        cloud_temp_ptr->push_back(grid[i][j].grid_cloud_ptr->points[m]);      //地面候选点云
                    }       
                }
                else
                {
                    for(int m=0; m<grid[i][j].grid_cloud_ptr->size(); ++m)
                    {
                        cloud_obstacle_ptr->push_back(grid[i][j].grid_cloud_ptr->points[m]);  //障碍物点云
                    }
                }
            }
        }
    }

    //应用随机采样一致性算法进一步提取地面点云
    pcl::ModelCoefficients::Ptr coefficients (new pcl::ModelCoefficients());
    pcl::PointIndices::Ptr inliers (new pcl::PointIndices());
    pcl::SACSegmentation<pcl::PointXYZ> seg;
    seg.setModelType (pcl::SACMODEL_PLANE);
    seg.setMethodType (pcl::SAC_RANSAC);
    seg.setOptimizeCoefficients (true);
    seg.setMaxIterations(500);
    seg.setDistanceThreshold (RANSAC_threshold);
    seg.setInputCloud (cloud_temp_ptr);
    seg.segment (*inliers, *coefficients);
    pcl::ExtractIndices<pcl::PointXYZ> extract;
    extract.setInputCloud(cloud_temp_ptr);
    extract.setIndices(inliers);
    extract.setNegative(false);
    extract.filter(*cloud_ground_ptr);

    int len = cloud_ground_ptr->size();
    double sum = 0, ave = 0;
    for (int i=0; i<len; ++i)
    {
        sum += cloud_ground_ptr->points[i].z;
    }
    ave = sum/len;
    std_msgs::Float32 ground_z;
    ground_z.data = ave;
    ground_z_pub.publish(ground_z);  //发布地面高度

    extract.setNegative (true);
    extract.filter (*cloud_temp2_ptr);
    for(int m=0; m<cloud_temp2_ptr->size(); ++m)
    {
        cloud_obstacle_ptr->push_back(cloud_temp2_ptr->points[m]);
    }

    //在障碍物点云中加入超声波数据点
    pcl::PointXYZ ultrasonic_point;
    ultrasonic_point.z = 0;
    for(int i=0; i<4; ++i){
        switch (i)
        {
        case 0: ultrasonic_point.x = 0.9;   break;
        case 1: ultrasonic_point.x = 0.65;  break;
        case 2: ultrasonic_point.x = -0.3;  break;
        case 3: ultrasonic_point.x = -1.05; break;
        default: break;
        }
        if(left_ultrasonic[i] > 0 && left_ultrasonic[i] < 5.0){
            ultrasonic_point.y = 0.75 + left_ultrasonic[i];
            cloud_obstacle_ptr->push_back(ultrasonic_point);
        }
        if(right_ultrasonic[i] > 0 && right_ultrasonic[i] < 5.0){
            ultrasonic_point.y = -0.75 - right_ultrasonic[i];
            cloud_obstacle_ptr->push_back(ultrasonic_point);
        }
    }

    //建立一个TH个通道的障碍物列表
    Grid obstacle[TH];  
    int num = 0;
    float min = 0;
    int min_idx = 0;
    for (int m=0; m<cloud_obstacle_ptr->size(); ++m)   //遍历障碍物点云
    {
        if(x_backward == 0)
        {
            if(cloud_obstacle_ptr->points[m].x < 0)
            {
                continue;  //跳过后部区域
            }
        }
        
        r = sqrt(cloud_obstacle_ptr->points[m].x * cloud_obstacle_ptr->points[m].x
                 + cloud_obstacle_ptr->points[m].y * cloud_obstacle_ptr->points[m].y);  //点到原点的距离
        th = acos(cloud_obstacle_ptr->points[m].x/r);  //点对应的向量的角度（以x轴正方向为零角度）
        
        if(cloud_obstacle_ptr->points[m].y<0)
        {
            th = 2*M_PI-th;
        }

        t = (int)floor(th/grid_size_th) % TH;
        obstacle[t].grid_cloud_ptr->points.push_back(cloud_obstacle_ptr->points[m]);  //把障碍物点划分到对应的小扇形内（TH等分）
    }
////////////////////////////////////通过障碍物点云提取可通行区域边界////////////////////////////////
    for(int j=0; j<TH; ++j)  //遍历障碍物列表obstacle
    {
        if(x_backward == 0)
        {
            if(j>=TH/4 && j<TH/4*3)
            {
                continue;  //跳过后部区域
            }
        }
        
        num = obstacle[j].grid_cloud_ptr->size();

        if(num == 0)  //该扇形区域无障碍物点
        {
            pcl::PointCloud<pcl::PointXYZ>::Ptr edge(new pcl::PointCloud<pcl::PointXYZ>);
            edge->resize(1);
            edge->points[0].x = radius*cos(j*grid_size_th);
            edge->points[0].y = radius*sin(j*grid_size_th);
            edge->points[0].z = ave;
            cloud_boundary_ptr->points.push_back(edge->points[0]);
            for(int i=0; i<R; ++i)
            {
                grid[i][j].IsDrivable = true;
            }
            continue;
        }

        std::vector<float> array;
        array.resize(num);
        for(int i=0; i<num; ++i)
        {
            array[i] = fabs(obstacle[j].grid_cloud_ptr->points[i].x) + fabs(obstacle[j].grid_cloud_ptr->points[i].y);
            if(i == 0)
            {
                min = array[i];
                min_idx = i;
            }
            if(array[i] < min)
            {
                min = array[i];
                min_idx = i;
            }
        }

        obstacle[j].grid_cloud_ptr->points[min_idx].z = ave;
        cloud_boundary_ptr->points.push_back(obstacle[j].grid_cloud_ptr->points[min_idx]);     //取每个小扇形区域内最近的障碍物点为边界点

        r = sqrt(obstacle[j].grid_cloud_ptr->points[min_idx].x * obstacle[j].grid_cloud_ptr->points[min_idx].x
                 + obstacle[j].grid_cloud_ptr->points[min_idx].y * obstacle[j].grid_cloud_ptr->points[min_idx].y);
        a = floor(r/grid_size_r);

        for(int i=0; i<a; ++i)
        {
            grid[i][j].IsDrivable = true;
        }
    }

////////////////////////////对极坐标可通行区域进行裁剪/////////////////////////////
    int cnt = 1;
    int left_flag = 0;
    int right_flag = 0;
    int temp_th = 0;
    int left_cut = 0;
    int right_cut = 0;
    for(int i=0; i<R; ++i)
    {
        for(int j=0; j<TH; ++j)
        {
            if(i < (int)(1.6/grid_size_r) || grid[i][j].IsDrivable==false)
            {
                continue;    //半径1.6m内不处理，非可行驶点不处理
            }
            if(grid[i][j].IsChecked)
            {
                continue;    //跳过已检查过的栅格
            }
            for(int m=1; i*grid_size_r*grid_size_th*cnt < cut_width; ++m)  //将宽度小于cut_width 的区域裁剪掉
            {//3
                if(left_flag == 0)
                {
                    temp_th = (j+m) % TH;
                    if(grid[i][temp_th].IsDrivable)
                    {
                        grid[i][temp_th].IsChecked = true;
                        cnt++;
                    }
                    else
                    {
                        left_cut = m-1;
                        left_flag = 1;
                    }
                }
                if(right_flag == 0)
                {
                    temp_th = (j-m+TH) % TH;
                    if(grid[i][temp_th].IsDrivable)
                    {
                        grid[i][temp_th].IsChecked = true;
                        cnt++;
                    }
                    else
                    {
                        right_cut = m-1;
                        right_flag = 1;
                    }
                }
                if(left_flag==1 && right_flag==1)  //宽度小于1.7m 则置为不可行驶点
                {
                    for(m=i; m<R; ++m)             //裁剪原栅格及其上部栅格
                    {
                        grid[m][j].IsDrivable = false;
                    }
                    for(m=1; m<=left_cut; ++m)     //裁剪原栅格左侧的栅格及其上部栅格
                    {
                        temp_th = (j+m) % TH;
                        for(int n=i; n<R; ++n)
                        {
                            grid[n][temp_th].IsDrivable = false;
                        }
                    }
                    for(m=1; m<=right_cut; ++m)    //裁剪原栅格右侧的栅格及其上部栅格
                    {
                        temp_th = (j-m+TH) % TH;
                        for(int n=i; n<R; ++n)
                        {
                            grid[n][temp_th].IsDrivable = false;
                        }
                    }
                    break;
                }
            }//3
            cnt = 1;
            left_flag = 0;
            right_flag = 0;
            left_cut = 0;
            right_cut = 0;
        }
    }

////////////////////////极坐标系栅格地图转化为直角坐标系可通行区域栅格地图////////////////////
    for(int j=-y_width; j<=y_width; ++j)
    {//1
        for(int i=-x_backward; i<=x_forward; ++i)
        {//2
            if(i==0 && j==0)
            {
                free_road.x = 0;
                free_road.y = 0;
                free_road.z = ave;
                grid_cell.cells.push_back(free_road);
                continue;
            }

            r = sqrt((float)(grid_size*i) * (float)(grid_size*i)
                     + (float)(grid_size*j) * (float)(grid_size*j));
            th = acos((float)(grid_size*i)/r);

            if(j < 0)
            {
                th = 2*M_PI-th;
            }

            t = (int)floor(th/grid_size_th) % TH;      //直角坐标系下的坐标转化为极坐标系的坐标，查询对应极坐标栅格的可通行性
            a = floor(r/grid_size_r);
            a = a >= R? R-1 : a;
            if(x_backward == 0)
            {
                if(t == TH/4)
                {
                    t = t-1;
                }
                else if(t == TH/4*3-1)
                {
                    t = TH/4*3;
                }
            }

            if(grid[a][t].IsDrivable)
            {
                free_road.x = grid_size*i;
                free_road.y = grid_size*j;
                free_road.z = ave;
                grid_cell.cells.push_back(free_road);
            }
        }//2   
    }//1

    grid_pub.publish(grid_cell);  //发布栅格地图
    clock_t end = clock();
    std_msgs::Float32 time;
    time.data = (float)(end-start)*1000/(float)CLOCKS_PER_SEC;  //程序用时 ms
    time_pub.publish(time);   //发布程序耗时
}

////////////////////////////////////////////主函数///////////////////////////////////////////////////
int main(int argc,char** argv)
{
    ros::init(argc,argv,"lidar_grid");
    LidarCloudHandler handler;
    ros::spin();
    return 0;
}