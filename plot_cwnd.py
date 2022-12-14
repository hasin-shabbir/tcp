import matplotlib.pyplot as plt
from argparse import ArgumentParser

parser = ArgumentParser(description="plot")

parser.add_argument('--path', '-p',
                    help="path to the log file",
                    required=True)

parser.add_argument('--name', '-n',
                    help="name of the output file",
                    required=True)

args = parser.parse_args()

# Parse the data from a csv file.
# Replace "data.csv" with the path to your csv file.
with open(args.path) as f:
    data = []
    for line in f:
        data.append(line.strip().split(","))

f.close()
# Extract the time and value columns from the data.
time = [float(x[0]) for x in data]
values = [float(x[1]) for x in data]

fig = plt.figure(figsize=(63,3), facecolor='w')
ax = plt.gca()
# Create a new figure and plot the time series data.
plt.plot(time, values)

# Add axis labels and show the plot.
plt.xlabel("Time")
plt.ylabel("CWND")
plt.grid(True, which="both")

plt.savefig(args.name+'.pdf',dpi=1000,bbox_inches='tight')
