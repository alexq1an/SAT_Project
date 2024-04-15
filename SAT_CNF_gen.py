import random

num_vars = 1000
num_clauses = 8000
solution = [random.choice([True, False]) for _ in range(num_vars)]

def generate_clause():
    clause = []

    # make sure first lit is valid
    rand_var = random.randint(1, num_vars)
    clause.append(rand_var if solution[rand_var-1] else -rand_var)
 
    while len(clause) < random.randint(2, 5):  # Choosing clause length between 5 and 10
        var = random.randint(1, num_vars)
        if var not in clause and -var not in clause:  # Avoid duplicates and contradictions
            # Randomly decide to add variable or its negation, independent of the solution
            if random.choice([True, False]):
                clause.append(var)
            else:
                clause.append(-var)
    clause.append(0)  # End of clause marker for DIMACS format
    return clause

if __name__ == "__main__":
    with open("sat_problem.cnf", "w") as file:
        file.write(f"p cnf {num_vars} {num_clauses}\n")
        for _ in range(num_clauses):
            file.write(" ".join(map(str, generate_clause())) + "\n")
