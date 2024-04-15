#include <iostream>
#include <vector>
#include <string>
#include <fstream> 
#include <sstream> // for std::istringstream
#include "core/get_time.h"
#include "core/utils.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <condition_variable>
#include <queue>
#include <map>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cassert>

#include<set>


#include <unordered_map>
#include <cmath> // For std::abs

#include <mpi.h>

// Define a Clause as a vector of integers, where each integer represents a variable
// Positive values denote the variable, and negative values denote its negation.
typedef std::vector<int> Clause;
// Define a Formula as a vector of Clauses
typedef std::vector<Clause> Formula;

std::atomic<bool> found_solution{false};
std::atomic<bool> all_workers_should_stop{false};

std::vector<int> completedTask;

struct Task {
    Formula formula;
    std::map<int, std::optional<bool>> assignment;

    Task(Formula f, std::map<int, std::optional<bool>> a)
        : formula(f), assignment(a) {}

    Task() = default;
};

// Serialize Task into a byte array
std::vector<char> serializeTask(const Task& task) {
    std::vector<char> buffer;

    // Serialize Formula
    size_t numClauses = task.formula.size();
    buffer.insert(buffer.end(), reinterpret_cast<const char*>(&numClauses), reinterpret_cast<const char*>(&numClauses + 1));
    for (const auto& clause : task.formula) {
        size_t clauseSize = clause.size();
        buffer.insert(buffer.end(), reinterpret_cast<const char*>(&clauseSize), reinterpret_cast<const char*>(&clauseSize + 1));
        for (int literal : clause) {
            buffer.insert(buffer.end(), reinterpret_cast<const char*>(&literal), reinterpret_cast<const char*>(&literal + 1));
        }
    }
    
    // Serialize Assignment Map
    size_t mapSize = task.assignment.size();
    buffer.insert(buffer.end(), reinterpret_cast<const char*>(&mapSize), reinterpret_cast<const char*>(&mapSize + 1));
    for (const auto& [key, opt_val] : task.assignment) {
        buffer.insert(buffer.end(), reinterpret_cast<const char*>(&key), reinterpret_cast<const char*>(&key + 1));
        bool hasValue = opt_val.has_value();
        buffer.insert(buffer.end(), reinterpret_cast<const char*>(&hasValue), reinterpret_cast<const char*>(&hasValue + 1));
        if (hasValue) {
            bool value = *opt_val;
            buffer.insert(buffer.end(), reinterpret_cast<const char*>(&value), reinterpret_cast<const char*>(&value + 1));
        }
    }

    return buffer;
}


// Deserialize Task from a byte array
Task deserializeTask(const std::vector<char>& buffer) {
    size_t pos = 0;
    Task task;
    
    // Deserialize the formula
    size_t numClauses = *reinterpret_cast<const size_t*>(buffer.data() + pos);
    pos += sizeof(size_t);
    task.formula.resize(numClauses);
    for (auto& clause : task.formula) {
        size_t clauseSize = *reinterpret_cast<const size_t*>(buffer.data() + pos);
        pos += sizeof(size_t);
        clause.resize(clauseSize);
        for (int& literal : clause) {
            literal = *reinterpret_cast<const int*>(buffer.data() + pos);
            pos += sizeof(int);
        }
    }
    
    // Deserialize the assignment map
    size_t mapSize = *reinterpret_cast<const size_t*>(buffer.data() + pos);
    pos += sizeof(size_t);
    for (size_t i = 0; i < mapSize; ++i) {
        int key = *reinterpret_cast<const int*>(buffer.data() + pos);
        pos += sizeof(int);
        bool hasValue = *reinterpret_cast<const bool*>(buffer.data() + pos);
        pos += sizeof(bool);
        std::optional<bool> opt_val;
        if (hasValue) {
            bool value = *reinterpret_cast<const bool*>(buffer.data() + pos);
            pos += sizeof(bool);
            opt_val = value;
        }
        task.assignment[key] = opt_val;
    }

    return task;
}



// MPI send and receive wrappers
void sendTask(const std::shared_ptr<Task>& taskPtr, int dest, int tag, MPI_Comm comm) {
    if (!taskPtr) {
        std::cerr << "Error: Attempted to send a null Task pointer." << std::endl;
        return;  // Optionally handle this case more gracefully
    }
    auto buffer = serializeTask(*taskPtr);  // Dereference the shared_ptr to access the Task
    int result = MPI_Send(buffer.data(), buffer.size(), MPI_CHAR, dest, tag, comm);
    if (result != MPI_SUCCESS) {
        // Handle MPI errors (e.g., print error message)
        char error_string[1024];
        int length_of_error_string;
        MPI_Error_string(result, error_string, &length_of_error_string);
        std::cerr << "MPI_Send failed: " << error_string << std::endl;
    }
}

std::shared_ptr<Task> recvTask(int source, int tag, MPI_Comm comm) {
    MPI_Status status;
    int num_bytes;
    MPI_Probe(source, tag, comm, &status);
    MPI_Get_count(&status, MPI_CHAR, &num_bytes);
    std::vector<char> buffer(num_bytes);
    MPI_Recv(buffer.data(), num_bytes, MPI_CHAR, source, tag, comm, &status);

    // Assuming deserializeTask returns a Task object
    Task* raw_task = new Task(deserializeTask(buffer));  // Allocate a new Task
    std::shared_ptr<Task> task_ptr(raw_task);  // Now manage this Task with shared_ptr
    return task_ptr;
}

class TaskQueue {
    std::queue<std::shared_ptr<Task>> queue;
    std::mutex mutex;
    std::condition_variable cond;


public:
    std::vector<int> completed_task;
    TaskQueue(uint n_thread): completed_task(n_thread){}

    void addTask(const std::shared_ptr<Task>& task) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(task);
        cond.notify_all();
    }

    std::shared_ptr<Task> getTask() {
        std::unique_lock<std::mutex> lock(mutex);
        // Wait until there is a task or it's time to stop all workers
        cond.wait(lock, [this] { return !queue.empty() || all_workers_should_stop.load(); });
        if (all_workers_should_stop.load()) {
            return nullptr; // Return nullptr if it's time to stop to ensure no thread is left waiting
        }
        if (!queue.empty()) {
            auto task = queue.front();
            queue.pop();
            return task;
        }
        return nullptr;
    }
    
    bool isEmpty() {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.empty();
    }

    void notifyAllWorkers() {
        std::lock_guard<std::mutex> lock(mutex);
        cond.notify_all();  // Wake up all threads
    }
};


std::queue<std::shared_ptr<Task>> taskQueue;  // Global task queue managed by the master

// Function to read a CNF file in DIMACS format and populate the formula
bool readDIMACSCNF(const std::string& filename, Formula& formula, int& numVariables) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Could not open file: " << filename << std::endl;
        return false;
    }

    std::string line;
    int numClauses;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == 'c') continue; // Skip comments and empty lines
        if (line[0] == 'p') {
            std::istringstream iss(line);
            std::string tmp;
            if (!(iss >> tmp >> tmp >> numVariables >> numClauses)) {
                std::cerr << "Error reading header line: " << line << std::endl;
                return false;
            }
            formula.reserve(numClauses); // Reserve space for clauses
            continue;
        }
        std::istringstream iss(line);
        Clause clause;
        int lit;
        while (iss >> lit && lit != 0) { // Read literals until 0
            clause.push_back(lit);
        }
        if (!clause.empty()) formula.push_back(clause);
    }
    return true;
}

// Helper function to get unassigned keys from the assignment map
std::set<int> getUnassignedKeys(const std::map<int, std::optional<bool>>& assignment) {
    std::set<int> keys;
    
    // Iterate over the map and check for keys with a std::nullopt value
    for (const auto& [key, val] : assignment) {
        if (!val.has_value()) {  // Equivalent to checking if val is std::nullopt
            keys.insert(key);
        }
    }
    // Sort the keys
    // std::sort(keys.begin(), keys.end());

    return keys;
}

// Simplify the formula based on the current assignments
Formula simplifyFormula(const Formula& formula, 
                        std::map<int, std::optional<bool>>& assignment) {
    Formula newFormula;

    for (const auto& clause : formula) {
        bool satisfied = false;
        for (int lit : clause) {
            if (lit > 0) {
                if (assignment.count(lit) && assignment[lit] == std::optional<bool>(true)) {
                    satisfied = true;
                    break;
                }
            } else {
                if (assignment.count(std::abs(lit)) && assignment[std::abs(lit)] == std::optional<bool>(false)) {
                    satisfied = true;
                    break;
                }
            }
        }
        if (!satisfied) {
            newFormula.push_back(clause);
        }
    }

    auto unassignedKeys = getUnassignedKeys(assignment);

    // Push all lit in the new Formula to keyInFormula
    std::vector<int> keyInFormula;
    for (const auto& clause : newFormula) {
        for (int lit : clause) {
            // If lit not already in keyInFormula, then push it in
            if (std::find(keyInFormula.begin(), keyInFormula.end(), lit) == keyInFormula.end()) {
                keyInFormula.push_back(lit);
            }
        }
    }

    // For each unassigned lit if its potive lit and negative lit all NOT found in varInFormula then set it to true (as we don't need it)
    for (int key : unassignedKeys) {
        if (std::find(keyInFormula.begin(), keyInFormula.end(), key) == keyInFormula.end() &&
            std::find(keyInFormula.begin(), keyInFormula.end(), -key) == keyInFormula.end()) {
            assignment[std::abs(key)] = std::optional<bool>(true);
        }
    }

    return newFormula;
}


uint countAssigned(const std::map<int, std::optional<bool>>& assignment){
    uint count = 0;
    for (const auto& [key, val] : assignment) {
        if (val.has_value()) {  // Equivalent to checking if val is std::nullopt
            count++;
        }
    }
    return count;
}

// See if by assigning clauses with only one missing value can satisfy, return true if unit clause is satisfiable
bool unitPropagation(Formula& formula, std::map<int, std::optional<bool>>& assignment) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& clause : formula) {
            int unassignedCount = 0;
            int lastUnassignedLit = 0;
            for (int lit : clause) {
                // Count the unassigned lit number in the clause 
                if (!assignment[std::abs(lit)].has_value()) {
                    unassignedCount++;
                    lastUnassignedLit = lit;
                } 
                // Else see if the clause satisfy, if statisfied set count to -1
                else if ((lit > 0 && assignment[std::abs(lit)] == true) ||
                           (lit < 0 && assignment[std::abs(lit)] == false)) {
                    unassignedCount = -1; // Clause is already satisfied, to next clause
                    break;
                }
            }
            // If only 1 unassigned lit in the clause, assign the value
            if (unassignedCount == 1) { // This is a unit clause
                assignment[std::abs(lastUnassignedLit)] = (lastUnassignedLit > 0);
                changed = true;
            } 
            // Else if clause cannot be satisfied
            else if (unassignedCount == 0) {
                // std::cout << "Dead end" << "\n";
                return false; // Clause cannot be satisfied, then means this path FAILS
            }
        }
    }
    return true;
}

void pureLiteralElimination(Formula& formula, std::map<int, std::optional<bool>>& assignment) {
    // std::map<int, int> polarity;
    // for (auto& clause : formula) {
    //     for (int lit : clause) {
    //         if (!assignment[std::abs(lit)].has_value()) {
    //             polarity[lit]++;
    //         }
    //     }
    // }

    // for (auto& p : polarity) {
    //     if (polarity[-p.first] == 0 && p.second != 0) { // Pure literal found
    //         assignment[std::abs(p.first)] = (p.first > 0);
    //     }
    // }


    std::map<int, int> polarity;
    std::set<int> consideredLiterals = getUnassignedKeys(assignment);
    for (auto& clause : formula) {
        for (int lit : clause) {
            int abs_lit = std::abs(lit);
            if (!assignment[abs_lit].has_value() && consideredLiterals.find(abs_lit) != consideredLiterals.end()) {
                polarity[lit]++;
                // consideredLiterals.insert(abs_lit);
            }
        }
    }

    // Test
    // for (auto& p : polarity) {
    //     // if (polarity[-p.first] == 0 && p.second != 0) { // Pure literal found
    //     //     assignment[std::abs(p.first)] = (p.first > 0);
    //     // }
    //     std::cout << p.first << "|" << p.second << "\n";
    // }

    for (int lit : consideredLiterals) {
        if (polarity[lit] > 0 && polarity[-lit] == 0) {  // Only positive literals are present
            assignment[std::abs(lit)] = true;
        } else if (polarity[-lit] > 0 && polarity[lit] == 0) {  // Only negative literals are present
            assignment[std::abs(lit)] = false;
        }
    }
}

bool isFormulaSatisfied(const Formula& formula, const std::map<int, std::optional<bool>>& assignment) {
    for (const auto& clause : formula) {
        bool satisfied = false;
        for (int lit : clause) {
            if ((lit > 0 && assignment.at(std::abs(lit)) == true) ||
                (lit < 0 && assignment.at(std::abs(lit)) == false)) {
                satisfied = true;
                break;
            }
        }
        if (!satisfied) return false;
    }
    return true;
}

void makeDecisionAndSpawn(std::shared_ptr<Task> task) {
    // Find the first unassigned variable
    int variable = -1;
    for (const auto& [var, val] : task->assignment) {
        if (!val.has_value()) {
            variable = var;
            break;
        }
    }

    if (variable != -1) {
        // Create two new nodes for each possible value of the variable
        for (bool val : {true, false}) {
            std::map<int, std::optional<bool>> newAssignment = task->assignment;
            newAssignment[variable] = val;

            Formula newFormula = task->formula; // Copy formula to potentially simplify
            // if (!unitPropagation(newFormula, newAssignment)) continue; // Skip unsatisfiable path

            std::shared_ptr<Task> newTask = std::make_shared<Task>(newFormula, newAssignment);
            // taskQueue.addTask(newNode);
            // Send the new task over
            sendTask(newTask, 0, 2, MPI_COMM_WORLD); // tag 2 means new task submission
        }
    } 
    // If no available variable found
    // else {
    //     // All variables are assigned, check if the formula is satisfied
    //     std::cout << "Dead end" << "\n";
    // }
    return;
}


bool handleTask(std::shared_ptr<Task> task){
        // std::cout << "START\n";
        // for (const auto& [var, val] : task->assignment) {
        //     if (val.has_value()) {
        //         std::cout << "Variable " << var << " = " << (val.value() ? "True" : "False") << "\n";
        //     }
        // }

        // PROCESS THE TASK
        // If the current assignment does not satisfy, then skip
        if (!unitPropagation(task->formula, task->assignment)) return false;

        // std::cout << "AFTER UNITPROP\n";
        // for (const auto& [var, val] : task->assignment) {
        //     if (val.has_value()) {
        //         std::cout << "Variable " << var << " = " << (val.value() ? "True" : "False") << "\n";
        //     }
        // }

        // Simplfy the form
        task->formula = simplifyFormula(task->formula, task->assignment);

        // Liminate all pure literal
        pureLiteralElimination(task->formula, task->assignment);

        // std::cout << "AFTER PUREELIM\n";
        // for (const auto& [var, val] : task->assignment) {
        //     if (val.has_value()) {
        //         std::cout << "Variable " << var << " = " << (val.value() ? "True" : "False") << "\n";
        //     }
        // }

        // Simplfy the form
        task->formula = simplifyFormula(task->formula, task->assignment);

        if (isFormulaSatisfied(task->formula, task->assignment)) {
            // std::cout << "SATISFIABLE\n";
            // for (const auto& [var, val] : task->assignment) {
            //     if (val.has_value()) {
            //         std::cout << "Variable " << var << " = " << (val.value() ? "True" : "False") << "\n";
            //     }
            // }

            return true;
        }
        
        makeDecisionAndSpawn(task);
        return false;
}

void master(uint world_size) {
    MPI_Status status;
    int flag;
    std::shared_ptr<Task> task;

    std::vector<bool> active(world_size, true);  // Track active workers.
    int active_count = world_size - 1;

    int remaining_workers = world_size-1;

    bool all_tasks_should_terminate = false;

    while (remaining_workers > 0) {

        // Check for any incoming message
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

        // if (taskQueue.completed_task[thread_id] % 1000 == 0){
        //     std::cout << "Thread number  " << thread_id << ", numOfTask: "  << taskQueue.completed_task[thread_id] << "\n";
        //     std::cout << "Assigned number:  " << countAssigned(task->assignment) << "\n";
        // }
        if (status.MPI_TAG == 1 && all_tasks_should_terminate) {
            int source = status.MPI_SOURCE;
            // std::cout << "Worker " << source << " want to term" << std::endl;

            int dummy;
            MPI_Recv(&flag, 1, MPI_INT, source, 1, MPI_COMM_WORLD, &status);  // Dummy receive to complete the probe

            int signal = -1;  // Let's use -1 as a termination code.
            MPI_Send(&signal, 1, MPI_INT, source, 3, MPI_COMM_WORLD);  // Using tag 3 for termination.

            remaining_workers--;
            continue;
        }
        // Determine message type based on the tag
        else if (status.MPI_TAG == 1) {  // Tag 1 means task request
            int source = status.MPI_SOURCE;
            MPI_Recv(&flag, 1, MPI_INT, source, 1, MPI_COMM_WORLD, &status);  // Dummy receive to complete the probe
            // Send a task if available
            if (!taskQueue.empty()) {
                task = taskQueue.front();
                taskQueue.pop();

                // Count 
                completedTask[source]++;
                sendTask(task, source, 0, MPI_COMM_WORLD);  // Tag 0 means sending a task
            } else {
                // Send a no-task signal, for example, by sending a special task or an empty message with a specific tag
                MPI_Send(&flag, 0, MPI_INT, source, 2, MPI_COMM_WORLD);  // Tag 2 means no task available
            }
        }
        else if (status.MPI_TAG == 2) {  // Tag 2 means new task submission
            task = recvTask(status.MPI_SOURCE, 2, MPI_COMM_WORLD);
            taskQueue.push(task);
        }
        else if (status.MPI_TAG == 3 && !all_tasks_should_terminate) {  // Handle other types of messages, like termination
            int source = status.MPI_SOURCE;
            MPI_Recv(&flag, 3, MPI_INT, source, 3, MPI_COMM_WORLD, &status);

            std::cout << "Task completed" << "\n";
            all_tasks_should_terminate = true;
            remaining_workers--;
            
            std::shared_ptr<Task> task = recvTask(status.MPI_SOURCE, 4, MPI_COMM_WORLD);

            std::cout << "SATISFIABLE\n";
            for (const auto& [var, val] : task->assignment) {
                if (val.has_value()) {
                    std::cout << "Variable " << var << " = " << (val.value() ? "True" : "False") << "\n";
                }
            }
        }
        else if (status.MPI_TAG == 3){
            int source = status.MPI_SOURCE;
            // Dummy recv terminate request
            MPI_Recv(&flag, 3, MPI_INT, source, 3, MPI_COMM_WORLD, &status);
            // Dunmmy recv task
            std::shared_ptr<Task> task = recvTask(status.MPI_SOURCE, 4, MPI_COMM_WORLD);
            remaining_workers--;
        }
    }
    // Stat
    for (uint i = 1; i < world_size; i++){
        std::cout << "Thread number " << i << " finished " << completedTask[i] << " tasks." << "\n";
    }
}

// To Master                            To Woker
// Tag 0:                               New task recieved
// Tag 1: Task request
// Tag 2: New task recieved             No Available task
// Tag 3: Compelete                     Compelete

void worker(uint rank, uint word_size) {
    while (true) {
        // Request a task from the master
        int flag = 1;  // Dummy flag to signal a request
        MPI_Send(&flag, 1, MPI_INT, 0, 1, MPI_COMM_WORLD);  // Send request to master (rank 0) with tag 1

        // Receive the task or a signal that no task is available
        MPI_Status status;
        MPI_Probe(0, MPI_ANY_TAG, MPI_COMM_WORLD, &status);  // Check source and tag

        if (status.MPI_TAG == 0) {  // Assuming tag 0 means a task is sent
            std::shared_ptr<Task> task = recvTask(0, 0, MPI_COMM_WORLD);
            // std::cout << "Worker " << rank << " received a task." << std::endl;
            
            // If found the solution terminate all threads
            // Process the task
            if (handleTask(task)){
                MPI_Send(&flag, 1, MPI_INT, 0, 3, MPI_COMM_WORLD);  // Send termination signal to master (rank 0) with tag 3
                // Send the task to master to print
                sendTask(task, 0, 4, MPI_COMM_WORLD);
                break;
            }
            // std::cout << "Worker " << rank << " completed processing the task." << std::endl;

        }
        else if (status.MPI_TAG == 2) {  // Tag 2 means no task right now
            int dummy;
            MPI_Recv(&dummy, 0, MPI_INT, 0, 2, MPI_COMM_WORLD, &status);  // Receive the no-task message
        }
        else if (status.MPI_TAG == 3) {
            int dummy;
            MPI_Recv(&dummy, 1, MPI_INT, 0, 3, MPI_COMM_WORLD, &status);
            // std::cout << "Worker " << rank << ": Received termination signal." << std::endl;
            break;  // Exit the loop
        }
    }
    // std::cout << thread_id << std::endl;
}

int main(int argc, char *argv[]) {

    std::string filename = "sat_problem.cnf"; 
    Formula formula;
    int numVariables = 0;

    // Read the CNF file
    if (!readDIMACSCNF(filename, formula, numVariables)) {
        std::cerr << "Failed to read CNF file." << std::endl;
        return 1;
    }

    timer t_mpi;
    t_mpi.start();

    std::map<int, std::optional<bool>> initial_assignment;

    // Initialize the map with keys from 1 to numVariables, all values set to std::nullopt
    for (int i = 1; i <= numVariables; ++i) {
        initial_assignment[i] = std::nullopt;
    }

    std::shared_ptr<Task> root = std::make_shared<Task>(formula, initial_assignment);
    taskQueue.push(root);

    MPI_Init(NULL, NULL);

    // Get the number of processes
    int world_size;
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    completedTask.resize(world_size);

    // Get the rank of the process
    int world_rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);

    if (world_size < 2 && world_rank == 0){
        std::cout << "Need at least two thread to maintain queue" << "\n";
        return -1;
    }

    // Get the name of the processor
    char processor_name[MPI_MAX_PROCESSOR_NAME];
    int name_len;
    MPI_Get_processor_name(processor_name, &name_len);

    if (world_rank == 0){
        std::cout << "Number of processes : " << world_size << "\n";
    }

    // Start master threads
    if (world_rank == 0){
        master(world_size);
    }
    else{
        // Start worker threads
        worker(world_rank, world_size);
    }

    double parallelTime = t_mpi.stop();

    if (world_rank == 0){
        std::cout << "MPI execution time used : " << parallelTime << " seconds"<< std::endl;
        std::cout << "All tasks completed. Program terminating." << std::endl;
    }

    MPI_Finalize();

    return 0;
}
