#include <eigen3/Eigen/Dense>
#include <iomanip>
#include <sys/stat.h>
#include <iterator>
#include "backend/problem.h"
#include "utility/tic_toc.h"

#define USE_OPENMP

#ifdef USE_OPENMP

#include <omp.h>

#endif

using namespace std;

// 针对 MatXX类型和 VecX 等复杂类型, 实现 自定义 reduction 
#pragma omp declare reduction (+: VecX: omp_out=omp_out+omp_in)\
     initializer(omp_priv=VecX::Zero(omp_orig.size()))
#pragma omp declare reduction (+: MatXX: omp_out=omp_out+omp_in)\
     initializer(omp_priv=MatXX::Zero(omp_orig.rows(), omp_orig.cols()))
//#endi

// define the format you want, you only need one instance of this...
const static Eigen::IOFormat CSVFormat(Eigen::StreamPrecision, Eigen::DontAlignCols, ", ", "\n");

void writeToCSVfile(std::string name, Eigen::MatrixXd matrix) {
    std::ofstream f(name.c_str());
    f << matrix.format(CSVFormat);
}

namespace myslam {
namespace backend {
void Problem::LogoutVectorSize() {
    // LOG(INFO) <<
    //           "1 problem::LogoutVectorSize verticies_:" << verticies_.size() <<
    //           " edges:" << edges_.size();
}

Problem::Problem(ProblemType problemType) :
    problemType_(problemType) {
    LogoutVectorSize();
    verticies_marg_.clear();
}

Problem::~Problem() {
//    std::cout << "Problem IS Deleted"<<std::endl;
    global_vertex_id = 0;
}

bool Problem::AddVertex(std::shared_ptr<Vertex> vertex) {
    if (verticies_.find(vertex->Id()) != verticies_.end()) {
        // LOG(WARNING) << "Vertex " << vertex->Id() << " has been added before";
        return false;
    } else {
        verticies_.insert(pair<unsigned long, shared_ptr<Vertex>>(vertex->Id(), vertex));
    }

    if (problemType_ == ProblemType::SLAM_PROBLEM) {
        if (IsPoseVertex(vertex)) {
            ResizePoseHessiansWhenAddingPose(vertex);
        }
    }
    return true;
}

void Problem::AddOrderingSLAM(std::shared_ptr<myslam::backend::Vertex> v) {
    if (IsPoseVertex(v)) {
        v->SetOrderingId(ordering_poses_);
        idx_pose_vertices_.insert(pair<ulong, std::shared_ptr<Vertex>>(v->Id(), v));
        ordering_poses_ += v->LocalDimension();
    } else if (IsLandmarkVertex(v)) {
        v->SetOrderingId(ordering_landmarks_);
        ordering_landmarks_ += v->LocalDimension();
        idx_landmark_vertices_.insert(pair<ulong, std::shared_ptr<Vertex>>(v->Id(), v));
    }
}

void Problem::ResizePoseHessiansWhenAddingPose(shared_ptr<Vertex> v) {

    int size = H_prior_.rows() + v->LocalDimension();
    H_prior_.conservativeResize(size, size);
    b_prior_.conservativeResize(size);

    b_prior_.tail(v->LocalDimension()).setZero();
    H_prior_.rightCols(v->LocalDimension()).setZero();
    H_prior_.bottomRows(v->LocalDimension()).setZero();

}
void Problem::ExtendHessiansPriorSize(int dim)
{
    int size = H_prior_.rows() + dim;
    H_prior_.conservativeResize(size, size);
    b_prior_.conservativeResize(size);

    b_prior_.tail(dim).setZero();
    H_prior_.rightCols(dim).setZero();
    H_prior_.bottomRows(dim).setZero();
}

bool Problem::IsPoseVertex(std::shared_ptr<myslam::backend::Vertex> v) {
    string type = v->TypeInfo();
    return type == string("VertexPose") ||
            type == string("VertexSpeedBias");
}

bool Problem::IsLandmarkVertex(std::shared_ptr<myslam::backend::Vertex> v) {
    string type = v->TypeInfo();
    return type == string("VertexPointXYZ") ||
           type == string("VertexInverseDepth");
}

bool Problem::AddEdge(shared_ptr<Edge> edge) {
    if (edges_.find(edge->Id()) == edges_.end()) {
        edges_.insert(pair<ulong, std::shared_ptr<Edge>>(edge->Id(), edge));
    } else {
        // LOG(WARNING) << "Edge " << edge->Id() << " has been added before!";
        return false;
    }

    for (auto &vertex: edge->Verticies()) {
        vertexToEdge_.insert(pair<ulong, shared_ptr<Edge>>(vertex->Id(), edge));
    }
    return true;
}

vector<shared_ptr<Edge>> Problem::GetConnectedEdges(std::shared_ptr<Vertex> vertex) {
    vector<shared_ptr<Edge>> edges;
    auto range = vertexToEdge_.equal_range(vertex->Id());
    for (auto iter = range.first; iter != range.second; ++iter) {

        // 并且这个edge还需要存在，而不是已经被remove了
        if (edges_.find(iter->second->Id()) == edges_.end())
            continue;

        edges.emplace_back(iter->second);
    }
    return edges;
}

bool Problem::RemoveVertex(std::shared_ptr<Vertex> vertex) {
    //check if the vertex is in map_verticies_
    if (verticies_.find(vertex->Id()) == verticies_.end()) {
        // LOG(WARNING) << "The vertex " << vertex->Id() << " is not in the problem!" << endl;
        return false;
    }

    // 这里要 remove 该顶点对应的 edge.
    vector<shared_ptr<Edge>> remove_edges = GetConnectedEdges(vertex);
    for (size_t i = 0; i < remove_edges.size(); i++) {
        RemoveEdge(remove_edges[i]);
    }

    if (IsPoseVertex(vertex))
        idx_pose_vertices_.erase(vertex->Id());
    else
        idx_landmark_vertices_.erase(vertex->Id());

    vertex->SetOrderingId(-1);      // used to debug
    verticies_.erase(vertex->Id());
    vertexToEdge_.erase(vertex->Id());

    return true;
}

bool Problem::RemoveEdge(std::shared_ptr<Edge> edge) {
    //check if the edge is in map_edges_
    if (edges_.find(edge->Id()) == edges_.end()) {
        // LOG(WARNING) << "The edge " << edge->Id() << " is not in the problem!" << endl;
        return false;
    }

    edges_.erase(edge->Id());
    return true;
}

bool Problem::Solve(int type, int iterations){
    switch (type)
    {
    case 0:
        return(SolveLM(iterations));
        break;
    case 1:
        return(SolveDogLeg(iterations));
        break;
    default:
        cerr << "Wrong option for solver type! Solver type should only be 0 or 1 !\n";
        return false;
        break;
    }
}

bool Problem::SolveDogLeg(int itertaions){
    if(edges_.size()==0 || verticies_.size() ==0){
        cerr << "\n Cannot solve problem without edges or vertices !\n";
        return false;
    }
    // 求解器计时
    TicToc t_solver;
    // 对变量进行排序（位姿在前，路标在后）
    SetOrdering();
    // 构建Hessian矩阵
    MakeHessian();
    // 初始化chi和radius
    ComputeRadiusInitDogLeg();
    // 迭代优化
    bool stop = false; // 是否停止迭代
    int iter = 0; // 当前迭代次数
    double last_chi = 0; // 上一次的误差，用于判断是否停止
    // 一直迭代到大于最大迭代次数或满足终止条件
    while((iter < itertaions) && !stop){
        // 输出当前结果
        std::cout << "iter: " << iter << " , chi= " << currentChi_ << " , currentRadius= " << currentRadius_ << std::endl;
        bool oneStepSuccess = false; // 当前迭代是否成功
        int false_cnt = 0; // 迭代失败次数
        // 多次尝试直到成功或大于最大尝试次数
        while(!oneStepSuccess && false_cnt < 10){
            // 求解delta_x
            SolveDogLegStep();
            // 更新状态
            UpdateStates();
            // 判断步长是否合适
            oneStepSuccess = IsGoodStepInDogLeg();
            // 迭代成功则保持状态更新，否则回滚
            if(oneStepSuccess){
                MakeHessian(); // 在新的线性化点计算Hessian
                false_cnt = 0; //计数器清零（好像不清零也无所谓？）
            }
            else{
                false_cnt++;
                RollbackStates();
            }
        }
        iter++; // 总迭代次数+1

        if(last_chi - currentChi_ < 1e-5 || b_.norm() < 1e-5){
            cout << "DogLeg: currnet chi small, found result.\n";
            stop = true;
        }

        last_chi = currentChi_;
    }

    solve_cost_ = t_solver.toc();
    saveCost({solve_cost_, t_hessian_cost_});
    // ofs_time_ << solve_cost << endl;
    std::cout << "problem solve cost: " << solve_cost_ << " ms" << std::endl;
    std::cout << "   makeHessian cost: " << t_hessian_cost_ << " ms" << std::endl;
    t_hessian_cost_ = 0.;
    return true;
}

bool Problem::SolveLM(int iterations) {


    if (edges_.size() == 0 || verticies_.size() == 0) {
        std::cerr << "\nCannot solve problem without edges or verticies" << std::endl;
        return false;
    }

    TicToc t_solve;
    // 统计优化变量的维数，为构建 H 矩阵做准备
    SetOrdering();
    // 遍历edge, 构建 H 矩阵
    MakeHessian();
    // LM 初始化
    ComputeLambdaInitLM();
    // LM 算法迭代求解
    bool stop = false;
    int iter = 0;
    double last_chi_ = 1e20;
    while (!stop && (iter < iterations)) {
        std::cout << "iter: " << iter << " , chi= " << currentChi_ << " , Lambda= " << currentLambda_ << std::endl;
        bool oneStepSuccess = false;
        int false_cnt = 0;
        while (!oneStepSuccess && false_cnt < 10)  // 不断尝试 Lambda, 直到成功迭代一步
        {
            // setLambda 该函数功能被移动到了SolveLinearSystem中
//            AddLambdatoHessianLM();
            // 第四步，解线性方程
            SolveLinearSystem();
            //
//            RemoveLambdaHessianLM();

            // 优化退出条件1： delta_x_ 很小则退出
//            if (delta_x_.squaredNorm() <= 1e-6 || false_cnt > 10)
            // TODO:: 退出条件还是有问题, 好多次误差都没变化了，还在迭代计算，应该搞一个误差不变了就中止
//            if ( false_cnt > 10)
//            {
//                stop = true;
//                break;
//            }

            // 更新状态量
            UpdateStates();
            // 判断当前步是否可行以及 LM 的 lambda 怎么更新, chi2 也计算一下
            oneStepSuccess = IsGoodStepInLM();
            // 后续处理，
            if (oneStepSuccess) {
//                std::cout << " get one step success\n";

                // 在新线性化点 构建 hessian
                MakeHessian();
                // TODO:: 这个判断条件可以丢掉，条件 b_max <= 1e-12 很难达到，这里的阈值条件不应该用绝对值，而是相对值
//                double b_max = 0.0;
//                for (int i = 0; i < b_.size(); ++i) {
//                    b_max = max(fabs(b_(i)), b_max);
//                }
//                // 优化退出条件2： 如果残差 b_max 已经很小了，那就退出
//                stop = (b_max <= 1e-12);
                false_cnt = 0;
            } else {
                false_cnt ++;
                RollbackStates();   // 误差没下降，回滚
            }
        }
        iter++;

        // 优化退出条件3： currentChi_ 跟第一次的 chi2 相比，下降了 1e6 倍则退出
        // TODO:: 应该改成前后两次的误差已经不再变化
//        if (sqrt(currentChi_) <= stopThresholdLM_)
//        if (sqrt(currentChi_) < 1e-15)
        if(last_chi_ - currentChi_ < 1e-5)
        {
            std::cout << "sqrt(currentChi_) <= stopThresholdLM_" << std::endl;
            stop = true;
        }
        last_chi_ = currentChi_;
    }

    solve_cost_ = t_solve.toc();
    saveCost({solve_cost_, t_hessian_cost_});
    // ofs_time_ << solve_cost << endl;
    // std::cout << "problem solve cost: " << solve_cost_ << " ms" << std::endl;
    // std::cout << "   makeHessian cost: " << t_hessian_cost_ << " ms" << std::endl;
    t_hessian_cost_ = 0.;
    return true;
}

void Problem::saveCost(initializer_list<double> times){

    // int n = times.size();
    
    FILE * fp;
    fp = fopen("./solver_cost.txt", "a+");
    
    for (auto ptr = times.begin(); ptr != times.end(); ++ptr){
        if(ptr != (times.end() - 1)){
            fprintf(fp, "%lf ", *ptr);
        }
        else{
            fprintf(fp, "%lf\n", *ptr);
        }
    }

    fclose(fp);
}

bool Problem::SolveGenericProblem(int iterations) {
    return true;
}

void Problem::SetOrdering() {

    // 每次重新计数
    ordering_poses_ = 0;
    ordering_generic_ = 0;
    ordering_landmarks_ = 0;

    // Note:: verticies_ 是 map 类型的, 顺序是按照 id 号排序的
    for (auto vertex: verticies_) {
        ordering_generic_ += vertex.second->LocalDimension();  // 所有的优化变量总维数

        if (problemType_ == ProblemType::SLAM_PROBLEM)    // 如果是 slam 问题，还要分别统计 pose 和 landmark 的维数，后面会对他们进行排序
        {
            AddOrderingSLAM(vertex.second);
        }

    }

    if (problemType_ == ProblemType::SLAM_PROBLEM) {
        // 这里要把 landmark 的 ordering 加上 pose 的数量，就保持了 landmark 在后,而 pose 在前
        ulong all_pose_dimension = ordering_poses_;
        for (auto landmarkVertex : idx_landmark_vertices_) {
            landmarkVertex.second->SetOrderingId(
                landmarkVertex.second->OrderingId() + all_pose_dimension
            );
        }
    }

//    CHECK_EQ(CheckOrdering(), true);
}

bool Problem::CheckOrdering() {
    if (problemType_ == ProblemType::SLAM_PROBLEM) {
        int current_ordering = 0;
        for (auto v: idx_pose_vertices_) {
            assert(v.second->OrderingId() == current_ordering);
            current_ordering += v.second->LocalDimension();
        }

        for (auto v: idx_landmark_vertices_) {
            assert(v.second->OrderingId() == current_ordering);
            current_ordering += v.second->LocalDimension();
        }
    }
    return true;
}

void Problem::MakeHessian(){
    int acc_opt = 1;
    switch (acc_opt)
    {
    case 0:
        MakeHessianSingle();
        break;
    case 1:
        MakeHessianMulti();
        break;
    case 2:
        MakeHessianOpenMP();
        break;
    }
}

void Problem::MakeHessianOpenMP(){
    TicToc t_h;
    // 直接构造大的 H 矩阵
    ulong size = ordering_generic_;
    MatXX H(MatXX::Zero(size, size));
    VecX b(VecX::Zero(size));

    // TODO:: accelate, accelate, accelate
    // 由于OpenMP不支持迭代器，需要存储所有edge的id用于后续循环调用
    for (auto& edge: edges_ ){
        edges_idx_.push_back( edge.first );
    }
    
    // 设置openmp所使用的线程数
    omp_set_num_threads(4);
    // // 设置Eigen所使用线程数，可选，默认为与OpenMP一致
    // Eigen::setNbThreads(4);
    // 指定OpenMP对for循环进行加速，由于Eigen对象不是标准对象，需要手动编写reduction
    // auto iter = edges_.begin();
    #pragma omp parallel for reduction(+: H) reduction(+: b) 
    for(unsigned int idx=0; idx < edges_.size(); idx++ ) {
        auto edge = edges_[edges_idx_[idx]];

        edge->ComputeResidual();
        edge->ComputeJacobians();

        // TODO:: robust cost
        auto jacobians = edge->Jacobians();
        auto verticies = edge->Verticies();
        assert(jacobians.size() == verticies.size());

        for (size_t i = 0; i < verticies.size(); ++i) {
            auto v_i = verticies[i];
            if (v_i->IsFixed()) continue;    // Hessian 里不需要添加它的信息，也就是它的雅克比为 0

            auto jacobian_i = jacobians[i];
            ulong index_i = v_i->OrderingId();
            ulong dim_i = v_i->LocalDimension();

            // 鲁棒核函数会修改残差和信息矩阵，如果没有设置 robust cost function，就会返回原来的
            double drho;
            MatXX robustInfo(edge->Information().rows(),edge->Information().cols());
            edge->RobustInfo(drho,robustInfo);

            MatXX JtW = jacobian_i.transpose() * robustInfo;
            for (size_t j = i; j < verticies.size(); ++j) {
                auto v_j = verticies[j];

                if (v_j->IsFixed()) continue;

                auto jacobian_j = jacobians[j];
                ulong index_j = v_j->OrderingId();
                ulong dim_j = v_j->LocalDimension();

                assert(v_j->OrderingId() != -1);
                MatXX hessian = JtW * jacobian_j;

                // 所有的信息矩阵叠加起来
                // 由于多线程对 H 的访问时不同块, 不会冲突, 可以不加 访问控制
                H.block(index_i, index_j, dim_i, dim_j).noalias() += hessian;
                if (j != i) {
                    // 对称的下三角
                    H.block(index_j, index_i, dim_j, dim_i).noalias() += hessian.transpose();
                }
            }
            b.segment(index_i, dim_i).noalias() -= drho * jacobian_i.transpose()* edge->Information() * edge->Residual();
        }
    }
    Hessian_ = H;
    b_ = b;
    t_hessian_cost_ += t_h.toc();

    if(H_prior_.rows() > 0)
    {
        MatXX H_prior_tmp = H_prior_;
        VecX b_prior_tmp = b_prior_;

        /// 遍历所有 POSE 顶点，然后设置相应的先验维度为 0 .  fix 外参数, SET PRIOR TO ZERO
        /// landmark 没有先验
        for (auto vertex: verticies_) {
           if (IsPoseVertex(vertex.second) && vertex.second->IsFixed() ) {
                int idx = vertex.second->OrderingId();
                int dim = vertex.second->LocalDimension();
                H_prior_tmp.block(idx,0, dim, H_prior_tmp.cols()).setZero();
                H_prior_tmp.block(0,idx, H_prior_tmp.rows(), dim).setZero();
                b_prior_tmp.segment(idx,dim).setZero();
//                std::cout << " fixed prior, set the Hprior and bprior part to zero, idx: "<<idx <<" dim: "<<dim<<std::endl;
            }
        }
        Hessian_.topLeftCorner(ordering_poses_, ordering_poses_) += H_prior_tmp;
        b_.head(ordering_poses_) += b_prior_tmp;
    }

    delta_x_ = VecX::Zero(size);  // initial delta_x = 0_n;

}

void Problem::MakeHessianMulti(){
    TicToc t_h;
    // 构造H矩阵和B矢量
    ulong size = ordering_generic_;
    multi_H_.setZero(size, size); // 变量清零
    multi_b_.setZero(size); // 变量清零

    // 获取所有边id
    for (auto& edge: edges_ ){
        edges_idx_.push_back( edge.first );
    }

    int thd_num = 4; // 线程数目
    vector<thread> all_thds; // 线程容器
    // 循环创建线程
    for (int i = 0; i < thd_num; i++){
        thread t = thread(std::mem_fn(&Problem::thdCalcHessian), this, i, thd_num);
        all_thds.emplace_back(move(t)); // 线程不能赋值只能使用move移动
    }
    // 等待所有线程完成Hessian计算后合并线程
    std::for_each(all_thds.begin(), all_thds.end(), std::mem_fn(&std::thread::join));
    
    // 赋值
    Hessian_ = multi_H_;
    b_ = multi_b_;
    t_hessian_cost_ += t_h.toc();
    // 后续代码与单线程相同

    if(H_prior_.rows() > 0)
    {
        MatXX H_prior_tmp = H_prior_;
        VecX b_prior_tmp = b_prior_;

        /// 遍历所有 POSE 顶点，然后设置相应的先验维度为 0 .  fix 外参数, SET PRIOR TO ZERO
        /// landmark 没有先验
        for (auto vertex: verticies_) {
           if (IsPoseVertex(vertex.second) && vertex.second->IsFixed() ) {
                int idx = vertex.second->OrderingId();
                int dim = vertex.second->LocalDimension();
                H_prior_tmp.block(idx,0, dim, H_prior_tmp.cols()).setZero();
                H_prior_tmp.block(0,idx, H_prior_tmp.rows(), dim).setZero();
                b_prior_tmp.segment(idx,dim).setZero();
//                std::cout << " fixed prior, set the Hprior and bprior part to zero, idx: "<<idx <<" dim: "<<dim<<std::endl;
            }
        }
        Hessian_.topLeftCorner(ordering_poses_, ordering_poses_) += H_prior_tmp;
        b_.head(ordering_poses_) += b_prior_tmp;
    }

    delta_x_ = VecX::Zero(size);  // initial delta_x = 0_n;

}
void Problem::thdCalcHessian(int thd_id, int thd_num){
    // 移动迭代器到起始位置
    // auto iter = edges_.begin();
    // advance(iter, thd_id);
    // 依次移动直到遍历完成
    // int cnt = thd_id;
    int edge_num = edges_.size();

    for(int i = thd_id; i < edge_num; i = i + thd_num){
        // printf("Thread %d, edge: %d/%d.\n", thd_id, cnt, edge_num);
        auto edge = edges_[edges_idx_[i]];
        edge->ComputeResidual();
        edge->ComputeJacobians();

        // TODO:: robust cost
        auto jacobians = edge->Jacobians();
        auto verticies = edge->Verticies();
        assert(jacobians.size() == verticies.size());

        for (size_t i = 0; i < verticies.size(); ++i) {
            auto v_i = verticies[i];
            if (v_i->IsFixed()) continue;    // Hessian 里不需要添加它的信息，也就是它的雅克比为 0

            auto jacobian_i = jacobians[i];
            ulong index_i = v_i->OrderingId();
            ulong dim_i = v_i->LocalDimension();

            // 鲁棒核函数会修改残差和信息矩阵，如果没有设置 robust cost function，就会返回原来的
            double drho;
            MatXX robustInfo(edge->Information().rows(),edge->Information().cols());
            edge->RobustInfo(drho,robustInfo);

            MatXX JtW = jacobian_i.transpose() * robustInfo;
            for (size_t j = i; j < verticies.size(); ++j) {
                auto v_j = verticies[j];

                if (v_j->IsFixed()) continue;

                auto jacobian_j = jacobians[j];
                ulong index_j = v_j->OrderingId();
                ulong dim_j = v_j->LocalDimension();

                assert(v_j->OrderingId() != -1);
                MatXX hessian = JtW * jacobian_j;

                // 所有的信息矩阵叠加起来
                multi_H_.block(index_i, index_j, dim_i, dim_j).noalias() += hessian;
                if (j != i) {
                    // 对称的下三角
                    multi_H_.block(index_j, index_i, dim_j, dim_i).noalias() += hessian.transpose();

                }
            }
            std::lock_guard<std::mutex> lock(m_hessian_);
            multi_b_.segment(index_i, dim_i).noalias() -= drho * jacobian_i.transpose()* edge->Information() * edge->Residual();
        }

        // cnt += thd_num;
        
        // if(cnt < edge_num){
        //     advance(iter, thd_num); // 移动迭代器
        // }
        // else{
        //     break;
        // }
    }
}

void Problem::MakeHessianSingle() {
    TicToc t_h;
    // 直接构造大的 H 矩阵
    ulong size = ordering_generic_;
    MatXX H(MatXX::Zero(size, size));
    VecX b(VecX::Zero(size));

    // TODO:: accelate, accelate, accelate
//#ifdef USE_OPENMP
//#pragma omp parallel for
//#endif
    for (auto &edge: edges_) {

        edge.second->ComputeResidual();
        edge.second->ComputeJacobians();

        // TODO:: robust cost
        auto jacobians = edge.second->Jacobians();
        auto verticies = edge.second->Verticies();
        assert(jacobians.size() == verticies.size());
        for (size_t i = 0; i < verticies.size(); ++i) {
            auto v_i = verticies[i];
            if (v_i->IsFixed()) continue;    // Hessian 里不需要添加它的信息，也就是它的雅克比为 0

            auto jacobian_i = jacobians[i];
            ulong index_i = v_i->OrderingId();
            ulong dim_i = v_i->LocalDimension();

            // 鲁棒核函数会修改残差和信息矩阵，如果没有设置 robust cost function，就会返回原来的
            double drho;
            MatXX robustInfo(edge.second->Information().rows(),edge.second->Information().cols());
            edge.second->RobustInfo(drho,robustInfo);

            MatXX JtW = jacobian_i.transpose() * robustInfo;
            for (size_t j = i; j < verticies.size(); ++j) {
                auto v_j = verticies[j];

                if (v_j->IsFixed()) continue;

                auto jacobian_j = jacobians[j];
                ulong index_j = v_j->OrderingId();
                ulong dim_j = v_j->LocalDimension();

                assert(v_j->OrderingId() != -1);
                MatXX hessian = JtW * jacobian_j;

                // 所有的信息矩阵叠加起来
                H.block(index_i, index_j, dim_i, dim_j).noalias() += hessian;
                if (j != i) {
                    // 对称的下三角
                    H.block(index_j, index_i, dim_j, dim_i).noalias() += hessian.transpose();

                }
            }
            b.segment(index_i, dim_i).noalias() -= drho * jacobian_i.transpose()* edge.second->Information() * edge.second->Residual();
        }

    }
    Hessian_ = H;
    b_ = b;
    t_hessian_cost_ += t_h.toc();

    if(H_prior_.rows() > 0)
    {
        MatXX H_prior_tmp = H_prior_;
        VecX b_prior_tmp = b_prior_;

        /// 遍历所有 POSE 顶点，然后设置需要fixed的顶点先验维度为 0 .  fix 外参数, SET PRIOR TO ZERO
        /// landmark 没有先验
        for (auto vertex: verticies_) {
            if (IsPoseVertex(vertex.second) && vertex.second->IsFixed() ) {
                int idx = vertex.second->OrderingId();
                int dim = vertex.second->LocalDimension();
                H_prior_tmp.block(idx,0, dim, H_prior_tmp.cols()).setZero();
                H_prior_tmp.block(0,idx, H_prior_tmp.rows(), dim).setZero();
                b_prior_tmp.segment(idx,dim).setZero();
//                std::cout << " fixed prior, set the Hprior and bprior part to zero, idx: "<<idx <<" dim: "<<dim<<std::endl;
            }
        }
        Hessian_.topLeftCorner(ordering_poses_, ordering_poses_) += H_prior_tmp;
        b_.head(ordering_poses_) += b_prior_tmp;
    }

    delta_x_ = VecX::Zero(size);  // initial delta_x = 0_n;


}

void Problem::SolveLinearWithSchur(MatXX & Hessian, VecX &b, VecX & delta_x, int reserve_size, int schur_size ,
                            std::map<unsigned long, std::shared_ptr<Vertex>> & schur_vertices, double lambda){
    // cout << "solve linear with schur, current pose_ordering is: " << ordering_poses_ << 
        // " and current landmark ordering is " << ordering_landmarks_ << endl;
    // 取Hessian矩阵的对应分块
    MatXX Hrr = Hessian.block(0, 0, reserve_size, reserve_size);
    MatXX Hss = Hessian.block(reserve_size, reserve_size, schur_size, schur_size);
    MatXX Hrs = Hessian.block(0, reserve_size, reserve_size, schur_size);
    MatXX Hsr = Hessian.block(reserve_size, 0, schur_size, reserve_size);
    // 取b的对应分块
    VecX brr = b.head(reserve_size);
    VecX bss = b.tail(schur_size);
    // 求Hss的逆()
    MatXX Hss_inv(MatXX::Zero(schur_size, schur_size));
    for (auto schurVertex : schur_vertices) {
        int idx = schurVertex.second->OrderingId() - reserve_size;
        int size = schurVertex.second->LocalDimension();
        Hss_inv.block(idx, idx, size, size) = Hss.block(idx, idx, size, size).inverse();
    }
    // schur complement
    MatXX tempH = Hrs * Hss_inv;
    MatXX Hrr_schur = Hrr - tempH * Hsr;
    VecX brr_schur = brr - tempH * bss;
    // 求解x_rr
    VecX x_rr(VecX::Zero(reserve_size));
 
    for(int i = 0; i < reserve_size; i++){
        Hrr_schur(i, i) += currentLambda_;
    }

    x_rr = Hrr_schur.ldlt().solve(brr_schur);
    delta_x.head(reserve_size) = x_rr;
    // 求解x_ss
    VecX x_ss(VecX::Zero(schur_size));
    x_ss = Hss_inv * (bss - Hsr * x_rr);
    delta_x.tail(schur_size) = x_ss;
}
/*
 * Solve Hx = b, we can use PCG iterative method or use sparse Cholesky
 */
void Problem::SolveLinearSystem() {


    if (problemType_ == ProblemType::GENERIC_PROBLEM) {
        // PCG solver
        MatXX H = Hessian_;
        for (size_t i = 0; i < Hessian_.cols(); ++i) {
            H(i, i) += currentLambda_;
        }
        // delta_x_ = PCGSolver(H, b_, H.rows() * 2);
        delta_x_ = H.ldlt().solve(b_);

    } else {
        
        //TicToc t_Hmminv;
        int reserve_size = ordering_poses_;
        int marg_size = ordering_landmarks_;
        
        SolveLinearWithSchur(Hessian_, b_, delta_x_, reserve_size, marg_size, idx_landmark_vertices_, currentLambda_);
    /*
        // 取各元素
        MatXX Hmm = Hessian_.block(reserve_size, reserve_size, marg_size, marg_size);
        MatXX Hpm = Hessian_.block(0, reserve_size, reserve_size, marg_size);
        MatXX Hmp = Hessian_.block(reserve_size, 0, marg_size, reserve_size);
        // MatXX Hmm = currentHessian.block(reserve_size, reserve_size, marg_size, marg_size);
        // MatXX Hpm = currentHessian.block(0, reserve_size, reserve_size, marg_size);
        // MatXX Hmp = currentHessian.block(reserve_size, 0, marg_size, reserve_size);
        VecX bpp = b_.segment(0, reserve_size);
        VecX bmm = b_.segment(reserve_size, marg_size);

        // Hmm 是对角线矩阵，它的求逆可以直接为对角线块分别求逆，如果是逆深度，对角线块为1维的，则直接为对角线的倒数，这里可以加速
        MatXX Hmm_inv(MatXX::Zero(marg_size, marg_size));
        // TODO:: use openMP
        for (auto landmarkVertex : idx_landmark_vertices_) {
            int idx = landmarkVertex.second->OrderingId() - reserve_size;
            int size = landmarkVertex.second->LocalDimension();
            Hmm_inv.block(idx, idx, size, size) = Hmm.block(idx, idx, size, size).inverse();
        }

        MatXX tempH = Hpm * Hmm_inv;
        // H_pp_schur_ = currentHessian.block(0, 0, ordering_poses_, ordering_poses_) - tempH * Hmp;
        H_pp_schur_ = Hessian_.block(0, 0, ordering_poses_, ordering_poses_) - tempH * Hmp;
        b_pp_schur_ = bpp - tempH * bmm;
        
        // step2: solve Hpp * delta_x = bpp
        VecX delta_x_pp(VecX::Zero(reserve_size));

        for (ulong i = 0; i < ordering_poses_; ++i) {
            H_pp_schur_(i, i) += currentLambda_;              // LM Method
        }

        // TicToc t_linearsolver;
        delta_x_pp =  H_pp_schur_.ldlt().solve(b_pp_schur_);//  SVec.asDiagonal() * svd.matrixV() * Ub;    
        delta_x_.head(reserve_size) = delta_x_pp;
        // std::cout << " Linear Solver Time Cost: " << t_linearsolver.toc() << std::endl;
        
        // step3: solve Hmm * delta_x = bmm - Hmp * delta_x_pp;
        VecX delta_x_ll(marg_size);
        delta_x_ll = Hmm_inv * (bmm - Hmp * delta_x_pp);
        delta_x_.tail(marg_size) = delta_x_ll;

        //std::cout << "schur time cost: "<< t_Hmminv.toc()<<std::endl;
    */
    }

}

void Problem::SolveDogLegStep(){
    // ----- 求解h_gn -----//
    // 对于普通问题，直接采用ldlt求解，
    // 对于SLAM问题，可采用schur Complement加速
    h_gn_ = VecX::Zero(delta_x_.size()); // 为h_gn_赋零值，主要是为了确定维数，否则Eigen操作VecX会报错

    if (problemType_ == ProblemType::GENERIC_PROBLEM) {
        // PCG solver
        MatXX H = Hessian_;
        // for (size_t i = 0; i < Hessian_.cols(); ++i) {
        //     H(i, i) += currentLambda_;
        // }
        // delta_x_ = PCGSolver(H, b_, H.rows() * 2);
        h_gn_ = H.ldlt().solve(b_);
    } else {
        int reserve_size = ordering_poses_;
        int schur_size = ordering_landmarks_;

        SolveLinearWithSchur(Hessian_, b_, h_gn_, reserve_size, schur_size, idx_landmark_vertices_, currentLambda_); 
    }
    // ----- 求解h_sd 和alpha
    alpha_ = b_.squaredNorm() / (b_.transpose() * Hessian_ * b_);
    h_sd_ = b_;
    // ----- 求解步长 ----- //
    double h_gn_norm = h_gn_.norm();
    double h_sd_norm = h_sd_.norm();

    if (h_gn_norm <= currentRadius_){
        h_dl_ = h_gn_;
    }
    else if(alpha_ * h_sd_norm >= currentRadius_){
        h_dl_ = (currentRadius_ / h_sd_norm) * h_sd_;
    }
    else{
       // ----- 求解beta ----- //
       VecX a = alpha_ * h_sd_;
       VecX b = h_gn_;
       double c = a.transpose() * (b - a);
       double sqrt_scale = sqrt(c * c + (b - a).squaredNorm() * (currentRadius_ * currentRadius_ - a.squaredNorm())); 
       if(c <= 0 ){
           beta_ = (-c + sqrt_scale) / (b - a).squaredNorm();
       } 
       else{
           beta_ = (currentRadius_ * currentRadius_ - a.squaredNorm()) / (c + sqrt_scale);
       }
       assert(beta_ > 0.0 && beta_ < 1.0);
       h_dl_ = a + beta_ * (b - a);
    }
    delta_x_ = h_dl_;
}

void Problem::UpdateStates() {

    // update vertex
    for (auto vertex: verticies_) {
        vertex.second->BackUpParameters();    // 保存上次的估计值

        ulong idx = vertex.second->OrderingId();
        ulong dim = vertex.second->LocalDimension();
        VecX delta = delta_x_.segment(idx, dim);
        vertex.second->Plus(delta);
    }

    // update prior
    if (err_prior_.rows() > 0) {
        // BACK UP b_prior_
        b_prior_backup_ = b_prior_;
        err_prior_backup_ = err_prior_;

        /// update with first order Taylor, b' = b + \frac{\delta b}{\delta x} * \delta x
        /// \delta x = Computes the linearized deviation from the references (linearization points)
        b_prior_ -= H_prior_ * delta_x_.head(ordering_poses_);       // update the error_prior
        err_prior_ = -Jt_prior_inv_ * b_prior_.head(ordering_poses_ - 15);

//        std::cout << "                : "<< b_prior_.norm()<<" " <<err_prior_.norm()<< std::endl;
//        std::cout << "     delta_x_ ex: "<< delta_x_.head(6).norm() << std::endl;
    }

}

void Problem::RollbackStates() {

    // update vertex
    for (auto vertex: verticies_) {
        vertex.second->RollBackParameters();
    }

    // Roll back prior_
    if (err_prior_.rows() > 0) {
        b_prior_ = b_prior_backup_;
        err_prior_ = err_prior_backup_;
    }
}

/// LM
void Problem::ComputeLambdaInitLM() {
    ni_ = 2.;
    currentLambda_ = -1.;
    currentChi_ = 0.0;

    for (auto edge: edges_) {
        currentChi_ += edge.second->RobustChi2();
    }
    if (err_prior_.rows() > 0)
        // currentChi_ += err_prior_.norm();
        currentChi_ += err_prior_.squaredNorm();
    currentChi_ *= 0.5;

    stopThresholdLM_ = 1e-10 * currentChi_;          // 迭代条件为 误差下降 1e-6 倍

    double maxDiagonal = 0;
    ulong size = Hessian_.cols();
    assert(Hessian_.rows() == Hessian_.cols() && "Hessian is not square");
    for (ulong i = 0; i < size; ++i) {
        maxDiagonal = std::max(fabs(Hessian_(i, i)), maxDiagonal);
    }

    maxDiagonal = std::min(5e10, maxDiagonal);
    double tau = 1e-5;  // 1e-5
    currentLambda_ = tau * maxDiagonal;
//        std::cout << "currentLamba_: "<<maxDiagonal<<" "<<currentLambda_<<std::endl;
}

// DogLeg 初始化chi和radius
void Problem::ComputeRadiusInitDogLeg(){
    // ----- 初始化Chi ----- //
    currentChi_ = 0.0;
    // 计算当前chi
    for(auto edge:edges_){
        // 此处不需要计算residual，因为MakeHessian时已经计算过
        currentChi_ += edge.second->RobustChi2();
    }
    // 计算先验chi
    if(err_prior_.rows() > 0){
        currentChi_ += err_prior_.squaredNorm();
        // currentChi_ += err_prior_.norm();
    }

    currentChi_ *= 0.5; // 0.5 * error^2

    stopThresholdDogLeg_ = 1e-15 * currentChi_;

    currentRadius_ = 1e4;
}

void Problem::AddLambdatoHessianLM() {
    ulong size = Hessian_.cols();
    assert(Hessian_.rows() == Hessian_.cols() && "Hessian is not square");
    for (ulong i = 0; i < size; ++i) {
        Hessian_(i, i) += currentLambda_;
    }
}

void Problem::RemoveLambdaHessianLM() {
    ulong size = Hessian_.cols();
    assert(Hessian_.rows() == Hessian_.cols() && "Hessian is not square");
    // TODO:: 这里不应该减去一个，数值的反复加减容易造成数值精度出问题？而应该保存叠加lambda前的值，在这里直接赋值
    for (ulong i = 0; i < size; ++i) {
        Hessian_(i, i) -= currentLambda_;
    }
}

bool Problem::IsGoodStepInLM() {
    double scale = 0;
    scale = 0.5 * delta_x_.transpose() * (currentLambda_ * delta_x_ + b_);
//    scale += 1e-3;    // make sure it's non-zero :)
    // scale = 0.5 * delta_x_.transpose() * (currentLambda_ * diagHessian_ * delta_x_ + b_); // 这里上下是否乘以0.5不会影响结果
    scale += 1e-6;    // make sure it's non-zero :)

    // recompute residuals after update state
    double tempChi = 0.0;
    for (auto edge: edges_) {
        edge.second->ComputeResidual();
        tempChi += edge.second->RobustChi2();
    }
    if (err_prior_.size() > 0)
        // 使用进行平方好像区别不大 ??
        // tempChi += err_prior_.norm();
        tempChi += err_prior_.squaredNorm();
    tempChi *= 0.5;          // 1/2 * err^2；上下同时乘以0.5不会影响结果，但需要同时！！

    double rho = (currentChi_ - tempChi) / scale;

    // // ---nielsen
    if (rho > 0 && isfinite(tempChi))   // last step was good, 误差在下降
    {
        double alpha = 1. - pow((2 * rho - 1), 3);
        alpha = std::min(alpha, 2. / 3.);
        double scaleFactor = (std::max)(1. / 3., alpha);
        currentLambda_ *= scaleFactor;
        ni_ = 2;
        currentChi_ = tempChi;
        return true;
    } else {
        currentLambda_ *= ni_;
        ni_ *= 2;
        return false;
    }
    // --- quadratic
    // double diff = currentChi_ - tempChi;
    // double h = b_.transpose() * delta_x_;
    // double alpha = h / (0.5 * diff + h);
    // if(rho > 0 && isfinite(tempChi)){
    //     currentLambda_ = std::max(currentLambda_ / (1 + alpha), 1e-7);
    //     currentChi_ = tempChi;
    // } else if(rho <= 0 && isfinite(tempChi)){
    //     currentLambda_ = currentLambda_ + std::abs(diff * 0.5 / alpha);
    //     currentChi_ = tempChi;
    // } else{
    //     return false;
    // }
    // // --- Marquat failed
    // if ( rho < 0.25 && isfinite(tempChi)) {
    //     currentLambda_ *= 2.0;
    //     currentChi_ = tempChi;
    //     return true;
    // }else if ( rho > 0.75 && isfinite(tempChi) ) {
    //     currentLambda_ /= 3.0;
    //     currentChi_ = tempChi;
    //     return true;    
    // } else {
    //     // do nothing
    //     return false;
    // }
}

bool Problem::IsGoodStepInDogLeg(){
    double tempChi = 0.;
    for(auto edge : edges_){
        // 由于执行过updateState，需要重新计算残差
        edge.second->ComputeResidual(); 
        tempChi += edge.second->RobustChi2();
    }
    // 先验残差
    if(err_prior_.size() > 0){
        tempChi += err_prior_.squaredNorm();
        // tempChi += err_prior_.norm();
    }

    tempChi *= 0.5; // 0.5 * error^2

    // 计算rho
    double rho = 0;
    double scale = 0.;
    int option = 0; // 选择论文策略或g20策略
    // 根据不同策略计算线性化模型误差
    switch (option)
    {
    case 0:
        if( h_dl_ == h_gn_ ){
            scale = currentChi_;
        } else if( h_dl_ == currentRadius_ * b_ / b_.norm()){
            scale = currentRadius_ * (2. * alpha_ * b_.norm() - currentRadius_) / (2. * alpha_);
        } else{
            scale = 0.5 * alpha_ * (1. - beta_) * (1. - beta_) * b_.squaredNorm() + beta_ * (2. - beta_) * currentChi_;
        }
        break;
    case 1:
        scale = -double(delta_x_.transpose() * Hessian_ * delta_x_) + 2 * b_.transpose() * delta_x_;
        break;
    }
    rho = (currentChi_ - tempChi) / scale;
    // 更新radius
     if (rho > 0.75 && isfinite(tempChi)) {
        currentRadius_ = std::max(currentRadius_, 3 * delta_x_.norm());
    }
    else if (rho < 0.25) {
        currentRadius_ = std::max(currentRadius_ * 0.5, 1e-7);
    } else {
        // do nothing
    }
    if (rho > 0 && isfinite(tempChi)) {
        currentChi_ = tempChi;
        return true;
    } else {
        return false;
    }
}

/** @brief conjugate gradient with perconditioning
 *
 *  the jacobi PCG method
 *
 */
VecX Problem::PCGSolver(const MatXX &A, const VecX &b, int maxIter = -1) {
    assert(A.rows() == A.cols() && "PCG solver ERROR: A is not a square matrix");
    int rows = b.rows();
    int n = maxIter < 0 ? rows : maxIter;
    VecX x(VecX::Zero(rows));
    MatXX M_inv = A.diagonal().asDiagonal().inverse();
    VecX r0(b);  // initial r = b - A*0 = b
    VecX z0 = M_inv * r0;
    VecX p(z0);
    VecX w = A * p;
    double r0z0 = r0.dot(z0);
    double alpha = r0z0 / p.dot(w);
    VecX r1 = r0 - alpha * w;
    int i = 0;
    double threshold = 1e-6 * r0.norm();
    while (r1.norm() > threshold && i < n) {
        i++;
        VecX z1 = M_inv * r1;
        double r1z1 = r1.dot(z1);
        double belta = r1z1 / r0z0;
        z0 = z1;
        r0z0 = r1z1;
        r0 = r1;
        p = belta * p + z1;
        w = A * p;
        alpha = r1z1 / p.dot(w);
        x += alpha * p;
        r1 -= alpha * w;
    }
    return x;
}

/*
 *  marg 所有和 frame 相连的 edge: imu factor, projection factor
 *  如果某个landmark和该frame相连，但是又不想加入marg, 那就把改edge先去掉
 *
 */
bool Problem::Marginalize(const std::vector<std::shared_ptr<Vertex> > margVertexs, int pose_dim) {

    SetOrdering();
    /// 找到需要 marg 的 edge, margVertexs[0] is frame, its edge contained pre-intergration
    std::vector<shared_ptr<Edge>> marg_edges = GetConnectedEdges(margVertexs[0]);

    std::unordered_map<int, shared_ptr<Vertex>> margLandmark;
    // 构建 Hessian 的时候 pose 的顺序不变，landmark的顺序要重新设定
    int marg_landmark_size = 0;
//    std::cout << "\n marg edge 1st id: "<< marg_edges.front()->Id() << " end id: "<<marg_edges.back()->Id()<<std::endl;
    for (size_t i = 0; i < marg_edges.size(); ++i) {
//        std::cout << "marg edge id: "<< marg_edges[i]->Id() <<std::endl;
        auto verticies = marg_edges[i]->Verticies();
        for (auto iter : verticies) {
            if (IsLandmarkVertex(iter) && margLandmark.find(iter->Id()) == margLandmark.end()) {
                iter->SetOrderingId(pose_dim + marg_landmark_size);
                margLandmark.insert(make_pair(iter->Id(), iter));
                marg_landmark_size += iter->LocalDimension();
            }
        }
    }
//    std::cout << "pose dim: " << pose_dim <<std::endl;
    int cols = pose_dim + marg_landmark_size;
    /// 构建误差 H 矩阵 H = H_marg + H_pp_prior
    MatXX H_marg(MatXX::Zero(cols, cols));
    VecX b_marg(VecX::Zero(cols));
    int ii = 0;
    for (auto edge: marg_edges) {
        edge->ComputeResidual();
        edge->ComputeJacobians();
        auto jacobians = edge->Jacobians();
        auto verticies = edge->Verticies();
        ii++;

        assert(jacobians.size() == verticies.size());
        for (size_t i = 0; i < verticies.size(); ++i) {
            auto v_i = verticies[i];
            auto jacobian_i = jacobians[i];
            ulong index_i = v_i->OrderingId();
            ulong dim_i = v_i->LocalDimension();

            double drho;
            MatXX robustInfo(edge->Information().rows(),edge->Information().cols());
            edge->RobustInfo(drho,robustInfo);

            for (size_t j = i; j < verticies.size(); ++j) {
                auto v_j = verticies[j];
                auto jacobian_j = jacobians[j];
                ulong index_j = v_j->OrderingId();
                ulong dim_j = v_j->LocalDimension();

                MatXX hessian = jacobian_i.transpose() * robustInfo * jacobian_j;

                assert(hessian.rows() == v_i->LocalDimension() && hessian.cols() == v_j->LocalDimension());
                // 所有的信息矩阵叠加起来
                H_marg.block(index_i, index_j, dim_i, dim_j) += hessian;
                if (j != i) {
                    // 对称的下三角
                    H_marg.block(index_j, index_i, dim_j, dim_i) += hessian.transpose();
                }
            }
            b_marg.segment(index_i, dim_i) -= drho * jacobian_i.transpose() * edge->Information() * edge->Residual();
        }

    }
        // std::cout << "edge factor cnt: " << ii <<std::endl;

    /// marg landmark
    int reserve_size = pose_dim;
    if (marg_landmark_size > 0) {
        int marg_size = marg_landmark_size;
        MatXX Hmm = H_marg.block(reserve_size, reserve_size, marg_size, marg_size);
        MatXX Hpm = H_marg.block(0, reserve_size, reserve_size, marg_size);
        MatXX Hmp = H_marg.block(reserve_size, 0, marg_size, reserve_size);
        VecX bpp = b_marg.segment(0, reserve_size);
        VecX bmm = b_marg.segment(reserve_size, marg_size);

        // Hmm 是对角线矩阵，它的求逆可以直接为对角线块分别求逆，如果是逆深度，对角线块为1维的，则直接为对角线的倒数，这里可以加速
        MatXX Hmm_inv(MatXX::Zero(marg_size, marg_size));
        // TODO:: use openMP
        for (auto iter: margLandmark) {
            int idx = iter.second->OrderingId() - reserve_size;
            int size = iter.second->LocalDimension();
            Hmm_inv.block(idx, idx, size, size) = Hmm.block(idx, idx, size, size).inverse();
        }

        MatXX tempH = Hpm * Hmm_inv;
        MatXX Hpp = H_marg.block(0, 0, reserve_size, reserve_size) - tempH * Hmp;
        bpp = bpp - tempH * bmm;
        H_marg = Hpp;
        b_marg = bpp;
    }

    VecX b_prior_before = b_prior_;
    if(H_prior_.rows() > 0)
    {
        H_marg += H_prior_;
        b_marg += b_prior_;
    }

    /// marg frame and speedbias
    int marg_dim = 0;

    // index 大的先移动
    for (int k = margVertexs.size() -1 ; k >= 0; --k)
    {

        int idx = margVertexs[k]->OrderingId();
        int dim = margVertexs[k]->LocalDimension();
//        std::cout << k << " "<<idx << std::endl;
        marg_dim += dim;
        // move the marg pose to the Hmm bottown right
        // 将 row i 移动矩阵最下面
        Eigen::MatrixXd temp_rows = H_marg.block(idx, 0, dim, reserve_size);
        Eigen::MatrixXd temp_botRows = H_marg.block(idx + dim, 0, reserve_size - idx - dim, reserve_size);
        H_marg.block(idx, 0, reserve_size - idx - dim, reserve_size) = temp_botRows;
        H_marg.block(reserve_size - dim, 0, dim, reserve_size) = temp_rows;

        // 将 col i 移动矩阵最右边
        Eigen::MatrixXd temp_cols = H_marg.block(0, idx, reserve_size, dim);
        Eigen::MatrixXd temp_rightCols = H_marg.block(0, idx + dim, reserve_size, reserve_size - idx - dim);
        H_marg.block(0, idx, reserve_size, reserve_size - idx - dim) = temp_rightCols;
        H_marg.block(0, reserve_size - dim, reserve_size, dim) = temp_cols;

        Eigen::VectorXd temp_b = b_marg.segment(idx, dim);
        Eigen::VectorXd temp_btail = b_marg.segment(idx + dim, reserve_size - idx - dim);
        b_marg.segment(idx, reserve_size - idx - dim) = temp_btail;
        b_marg.segment(reserve_size - dim, dim) = temp_b;
    }

    double eps = 1e-8;
    int m2 = marg_dim;
    int n2 = reserve_size - marg_dim;   // marg pose
    Eigen::MatrixXd Amm = 0.5 * (H_marg.block(n2, n2, m2, m2) + H_marg.block(n2, n2, m2, m2).transpose());

    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> saes(Amm);
    Eigen::MatrixXd Amm_inv = saes.eigenvectors() * Eigen::VectorXd(
            (saes.eigenvalues().array() > eps).select(saes.eigenvalues().array().inverse(), 0)).asDiagonal() *
                              saes.eigenvectors().transpose();

    Eigen::VectorXd bmm2 = b_marg.segment(n2, m2);
    Eigen::MatrixXd Arm = H_marg.block(0, n2, n2, m2);
    Eigen::MatrixXd Amr = H_marg.block(n2, 0, m2, n2);
    Eigen::MatrixXd Arr = H_marg.block(0, 0, n2, n2);
    Eigen::VectorXd brr = b_marg.segment(0, n2);
    Eigen::MatrixXd tempB = Arm * Amm_inv;
    H_prior_ = Arr - tempB * Amr;
    b_prior_ = brr - tempB * bmm2;

    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> saes2(H_prior_);
    Eigen::VectorXd S = Eigen::VectorXd((saes2.eigenvalues().array() > eps).select(saes2.eigenvalues().array(), 0));
    Eigen::VectorXd S_inv = Eigen::VectorXd(
            (saes2.eigenvalues().array() > eps).select(saes2.eigenvalues().array().inverse(), 0));
    // 从b中反解误差r
    Eigen::VectorXd S_sqrt = S.cwiseSqrt();
    Eigen::VectorXd S_inv_sqrt = S_inv.cwiseSqrt();
    Jt_prior_inv_ = S_inv_sqrt.asDiagonal() * saes2.eigenvectors().transpose();
    err_prior_ = -Jt_prior_inv_ * b_prior_;

    MatXX J = S_sqrt.asDiagonal() * saes2.eigenvectors().transpose();
    H_prior_ = J.transpose() * J;
    MatXX tmp_h = MatXX( (H_prior_.array().abs() > 1e-9).select(H_prior_.array(),0) );
    H_prior_ = tmp_h;

    // std::cout << "my marg b prior: " <<b_prior_.rows()<<" norm: "<< b_prior_.norm() << std::endl;
    // std::cout << "    error prior: " <<err_prior_.norm() << std::endl;

    // remove vertex and remove edge
    for (size_t k = 0; k < margVertexs.size(); ++k) {
        RemoveVertex(margVertexs[k]);
    }

    for (auto landmarkVertex: margLandmark) {
        RemoveVertex(landmarkVertex.second);
    }

    return true;

}

}
}






