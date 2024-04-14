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

#define DEFAULT_NUMBER_OF_THREADS "1"

// Define a Clause as a vector of integers, where each integer represents a variable
// Positive values denote the variable, and negative values denote its negation.
typedef std::vector<int> Clause;
// Define a Formula as a vector of Clauses
typedef std::vector<Clause> Formula;

std::atomic<bool> found_solution{false};
std::atomic<bool> all_workers_should_stop{false};

struct Task {
    Formula formula;
    std::map<int, std::optional<bool>> assignment;

    Task(Formula f, std::map<int, std::optional<bool>> a)
        : formula(f), assignment(a) {}
};

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

void makeDecisionAndSpawn(std::shared_ptr<Task> task, TaskQueue& taskQueue) {
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

            std::shared_ptr<Task> newNode = std::make_shared<Task>(newFormula, newAssignment);
            taskQueue.addTask(newNode);
        }
    } 
    // If no available variable found
    // else {
    //     // All variables are assigned, check if the formula is satisfied
    //     std::cout << "Dead end" << "\n";
    // }
    return;
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

void worker(TaskQueue& taskQueue, uint thread_id, uint n_threads) {
    while (!all_workers_should_stop.load()) {
        auto node = taskQueue.getTask();
        // Wait for task
        if (node == nullptr || found_solution.load()) {
            break; // Exit if no task or solution found
        }
        // Count the task given
        taskQueue.completed_task[thread_id]++;

        if (taskQueue.completed_task[thread_id] % 1000 == 0){
            std::cout << "Thread number  " << thread_id << ", numOfTask: "  << taskQueue.completed_task[thread_id] << "\n";
            std::cout << "Assigned number:  " << countAssigned(node->assignment) << "\n";
        }

        // std::cout << "START\n";
        // for (const auto& [var, val] : node->assignment) {
        //     if (val.has_value()) {
        //         std::cout << "Variable " << var << " = " << (val.value() ? "True" : "False") << "\n";
        //     }
        // }

        // PROCESS THE TASK
        // If the current assignment does not satisfy, then skip
        if (!unitPropagation(node->formula, node->assignment)) continue;

        // std::cout << "AFTER UNITPROP\n";
        // for (const auto& [var, val] : node->assignment) {
        //     if (val.has_value()) {
        //         std::cout << "Variable " << var << " = " << (val.value() ? "True" : "False") << "\n";
        //     }
        // }

        // Simplfy the form
        node->formula = simplifyFormula(node->formula, node->assignment);

        // Liminate all pure literal
        pureLiteralElimination(node->formula, node->assignment);

        // std::cout << "AFTER PUREELIM\n";
        // for (const auto& [var, val] : node->assignment) {
        //     if (val.has_value()) {
        //         std::cout << "Variable " << var << " = " << (val.value() ? "True" : "False") << "\n";
        //     }
        // }

        // Simplfy the form
        node->formula = simplifyFormula(node->formula, node->assignment);

        if (isFormulaSatisfied(node->formula, node->assignment)) {
            found_solution.store(true);
            all_workers_should_stop.store(true);
            taskQueue.notifyAllWorkers();  // notify all threads
            std::cout << "SATISFIABLE\n";
            for (const auto& [var, val] : node->assignment) {
                if (val.has_value()) {
                    std::cout << "Variable " << var << " = " << (val.value() ? "True" : "False") << "\n";
                }
            }
            // Stats
            for (uint i = 0; i < n_threads; i++){
                std::cout << "Thread number " << i << " finished " << taskQueue.completed_task[i] << " tasks." << "\n";
            }
            break;
        }
        
        makeDecisionAndSpawn(node, taskQueue);
    }
    // std::cout << thread_id << std::endl;
}

int main(int argc, char *argv[]) {

    cxxopts::Options options(
        "page_rank_push",
        "Calculate page_rank using serial and parallel execution");
    options.add_options(
        "",
        {
            {"nThreads", "Number of Threads",
            cxxopts::value<uint>()->default_value(DEFAULT_NUMBER_OF_THREADS)},
        });

    auto cl_options = options.parse(argc, argv);
    uint n_threads = cl_options["nThreads"].as<uint>();
    if (n_threads <= 0){
        std::cout << "Number of Threads cannot be less than 0" << std::endl;
        std::cout << "Exiting." << std::endl;
        return -1;
    }


    std::string filename = "sat_problem.cnf"; 
    Formula formula;
    int numVariables = 0;

    // Read the CNF file
    if (!readDIMACSCNF(filename, formula, numVariables)) {
        std::cerr << "Failed to read CNF file." << std::endl;
        return 1;
    }

    timer t_serial;
    t_serial.start();

    std::map<int, std::optional<bool>> initial_assignment;

    // Initialize the map with keys from 1 to numVariables, all values set to std::nullopt
    for (int i = 1; i <= numVariables; ++i) {
        initial_assignment[i] = std::nullopt;
    }

    std::shared_ptr<Task> root = std::make_shared<Task>(formula, initial_assignment);
    TaskQueue taskQueue(n_threads);
    taskQueue.addTask(root);

    // Start worker threads
    std::vector<std::thread> workers;
    for (int i = 0; i < n_threads; ++i) {
        workers.emplace_back(worker, std::ref(taskQueue), i, n_threads);
    }
    // Join threads
    for (auto& t : workers) {
        t.join();  
    }

    double serialTime = t_serial.stop();

    std::cout << "Serial execution time used : " << serialTime << " seconds"<< std::endl;

    std::cout << "All tasks completed. Program terminating." << std::endl;

    return 0;
}
