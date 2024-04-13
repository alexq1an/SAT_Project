#include <iostream>
#include <vector>
#include <string>
#include <fstream> 
#include <sstream> // for std::istringstream
#include "core/get_time.h"
#include <thread>
#include <atomic>
#include <vector>
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

// Define a Clause as a vector of integers, where each integer represents a variable
// Positive values denote the variable, and negative values denote its negation.
typedef std::vector<int> Clause;
// Define a Formula as a vector of Clauses
typedef std::vector<Clause> Formula;

std::atomic<bool> found_solution{false};
std::atomic<bool> all_workers_should_stop{false};

struct Node {
    Formula formula;
    std::map<int, std::optional<bool>> assignment;
    int variable;
    std::optional<bool> value;

    Node(Formula f, std::map<int, std::optional<bool>> a, int v, std::optional<bool> val)
        : formula(f), assignment(a), variable(v), value(val) {}
};

class TaskQueue {
    std::queue<std::shared_ptr<Node>> queue;
    std::mutex mutex;
    std::condition_variable cond;

public:
    void addTask(const std::shared_ptr<Node>& node) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(node);
        cond.notify_all();
    }

    std::shared_ptr<Node> getTask() {
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

bool unitPropagation(Formula& formula, std::map<int, std::optional<bool>>& assignment) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& clause : formula) {
            int unassignedCount = 0;
            int lastUnassignedLit = 0;
            for (int lit : clause) {
                if (!assignment[std::abs(lit)].has_value()) {
                    unassignedCount++;
                    lastUnassignedLit = lit;
                } else if ((lit > 0 && assignment[std::abs(lit)] == true) ||
                           (lit < 0 && assignment[std::abs(lit)] == false)) {
                    unassignedCount = -1; // Clause is already satisfied
                    break;
                }
            }
            if (unassignedCount == 1) { // This is a unit clause
                assignment[std::abs(lastUnassignedLit)] = (lastUnassignedLit > 0);
                changed = true;
            } else if (unassignedCount == 0) {
                return false; // Clause cannot be satisfied
            }
        }
    }
    return true;
}

void pureLiteralElimination(Formula& formula, std::map<int, std::optional<bool>>& assignment) {
    std::map<int, int> polarity;
    for (auto& clause : formula) {
        for (int lit : clause) {
            if (!assignment[std::abs(lit)].has_value()) {
                polarity[lit]++;
            }
        }
    }

    for (auto& p : polarity) {
        if (polarity[-p.first] == 0 && p.second != 0) { // Pure literal found
            assignment[std::abs(p.first)] = (p.first > 0);
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

void makeDecisionAndSpawn(std::shared_ptr<Node> node, TaskQueue& taskQueue) {
    // Find the first unassigned variable
    int variable = 0;
    for (const auto& [var, val] : node->assignment) {
        if (!val.has_value()) {
            variable = var;
            break;
        }
    }

    if (variable != 0) {
        // Create two new nodes for each possible value of the variable
        for (bool val : {true, false}) {
            std::map<int, std::optional<bool>> newAssignment = node->assignment;
            newAssignment[variable] = val;

            Formula newFormula = node->formula; // Copy formula to potentially simplify
            if (!unitPropagation(newFormula, newAssignment)) continue; // Skip unsatisfiable path

            std::shared_ptr<Node> newNode = std::make_shared<Node>(newFormula, newAssignment, variable, val);
            taskQueue.addTask(newNode);
        }
    } else {
        // All variables are assigned, check if the formula is satisfied
        if (isFormulaSatisfied(node->formula, node->assignment)) {
            found_solution = true;
            all_workers_should_stop.store(true);
            taskQueue.notifyAllWorkers();  // notify all threads
        }
    }
    return;
}

void worker(TaskQueue& taskQueue) {
    while (!all_workers_should_stop.load()) {
        auto node = taskQueue.getTask();
        if (node == nullptr || found_solution.load()) {
            break; // Exit if no task or solution found
        }
        // Process the task
        if (!unitPropagation(node->formula, node->assignment)) continue;
        pureLiteralElimination(node->formula, node->assignment);
        if (isFormulaSatisfied(node->formula, node->assignment)) {
            found_solution = true;
            all_workers_should_stop.store(true);
            taskQueue.notifyAllWorkers();  // notify all threads
            std::cout << "SATISFIABLE\n";
            for (const auto& [var, val] : node->assignment) {
                if (val.has_value()) {
                    std::cout << "Variable " << var << " = " << (val.value() ? "True" : "False") << "\n";
                }
            }
            break;
        }
        
        makeDecisionAndSpawn(node, taskQueue);
    }
}

int main() {
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

    std::shared_ptr<Node> root = std::make_shared<Node>(formula, initial_assignment, 0, std::nullopt);
    TaskQueue taskQueue;
    taskQueue.addTask(root);

    // Start worker threads
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.emplace_back(worker, std::ref(taskQueue));
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
