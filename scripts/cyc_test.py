def cyc_test(cycles: list[int])->list[int]:
    results = []
    for i in range(len(cycles) - 1):
        results.append(cycles[i+1]-cycles[i])
    return results

def main() :
    items : list[int] = [287211, 721451, 1446971, 2546571, 4103051, 6199211, 8917851, 12341771]

    while (len(items) > 0):
        print(items)
        items = cyc_test(items)

if __name__ == "__main__":
    main()

    