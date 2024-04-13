#include <iostream>
#include <vector>
#include <string>
#include <fstream> 
#include <sstream> // for std::istringstream
#include "core/get_time.h"
#include <thread>

// Define a Clause as a vector of integers, where each integer represents a variable
// Positive values denote the variable, and negative values denote its negation.
typedef std::vector<int> Clause;
// Define a Formula as a vector of Clauses
typedef std::vector<Clause> Formula;

std::atomic<bool> found_solution(false);
std::mutex io_mutex;

// Function to check if a given assignment of variables satisfies a clause
bool isClauseSatisfied(const Clause& clause, const std::vector<bool>& assignment, const std::vector<bool>& assigned) {
    for (int var : clause) {
        int index = abs(var) - 1; // Convert to 0-based index
        if (assigned[index]) { // Check if the variable has been assigned
            bool value = assignment[index]; // Get the assigned value
            if (var > 0 && value) return true; // Positive literal satisfied
            if (var < 0 && !value) return true; // Negative literal satisfied
        }
    }
    return false; // None of the literals in the clause are satisfied
}

bool unitPropagation(Formula &formula, std::vector<bool> &assignment, std::vector<bool> &assigned) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto &clause : formula) {
            // Count unassigned literals in the clause
            int unassignedCount = 0;
            int lastUnassignedLit = 0;
            for (int lit : clause) {
                int index = abs(lit) - 1;
                if (!assigned[index]) {
                    unassignedCount++;
                    lastUnassignedLit = lit;
                } else if ((lit > 0 && assignment[index]) || (lit < 0 && !assignment[index])) {
                    // Clause already satisfied
                    unassignedCount = -1;
                    break;
                }
            }
            if (unassignedCount == 1) {
                // We have a unit clause, assign the last unassigned literal
                int index = abs(lastUnassignedLit) - 1;
                assignment[index] = lastUnassignedLit > 0;
                assigned[index] = true;
                changed = true;
            } else if (unassignedCount == 0) {
                // Clause cannot be satisfied
                return false;
            }
        }
    }
    return true;
}

bool solveSAT(Formula& formula, std::vector<bool>& assignment, std::vector<bool>& assigned, int depth = 0) {
    // Apply unit propagation to simplify the formula
    if (!unitPropagation(formula, assignment, assigned)) {
        // If unitPropagation returns false, the formula is unsatisfiable with the current assignments
        return false;
    }

    if (depth == assignment.size()) { // All variables assigned, check if the formula is satisfied
        // After unit propagation, we might find the formula already satisfied before reaching this depth
        for (const Clause& clause : formula) {
            if (!isClauseSatisfied(clause, assignment, assigned)) return false;
        }
        return true; // Formula satisfied
    }

    // Try assigning true to the current variable
    assigned[depth] = true;
    assignment[depth] = true;
    if (solveSAT(formula, assignment, assigned, depth + 1)) return true;

    // Try assigning false to the current variable
    assignment[depth] = false;
    if (solveSAT(formula, assignment, assigned, depth + 1)) return true;

    // Backtrack
    assigned[depth] = false;
    return false;
}

void parallelSolveSAT(Formula formula, std::vector<bool> assignment, std::vector<bool> assigned, std::vector<int> decisions) {
    for (size_t i = 0; i < decisions.size(); ++i) {
        assigned[i] = true;
        assignment[i] = decisions[i];
    }
    bool result = solveSAT(formula, assignment, assigned, decisions.size());
    if (result && !found_solution.exchange(true)) { // Check and set found_solution atomically
        std::lock_guard<std::mutex> lock(io_mutex);
        std::cout << "SATISFIABLE. Assignment:" << std::endl;
        for (size_t i = 0; i < assignment.size(); ++i) {
            std::cout << "x" << i + 1 << " = " << (assignment[i] ? "True" : "False") << std::endl;
        }
    }
}

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


int main() {
    std::string filename = "sat_problem.cnf"; 
    Formula formula;
    int numVariables = 0;

    // Read the CNF file
    if (!readDIMACSCNF(filename, formula, numVariables)) {
        std::cerr << "Failed to read CNF file." << std::endl;
        return 1;
    }

    std::vector<bool> assignment(numVariables, false); // Current assignment of variables
    std::vector<bool> assigned(numVariables, false); // Track which variables have been assigned

    timer t;
    t.start();

    int parallelDepth = 3; // Number of variables to explore in parallel
    std::vector<std::thread> threads;

    // Creating 4 threads based on binary decisions for the first two variables
    for (int i = 0; i < (1 << parallelDepth); ++i) { // 1 << parallelDepth = 2^parallelDepth
        std::vector<int> decisions(parallelDepth);
        for (int j = 0; j < parallelDepth; ++j) {
            decisions[j] = (i & (1 << j)) != 0; // Determine the truth value for each decision
        }
        threads.emplace_back(parallelSolveSAT, formula, assignment, assigned, decisions);
    }

    // Join all threads
    for (auto& thread : threads) {
        thread.join();
    }

    if (!found_solution) {
        std::cout << "UNSATISFIABLE." << std::endl;
    }
    double time = t.stop();
    std::cout << "Parallel execution time used : " << time << " seconds"<< std::endl;
    return 0;
}
