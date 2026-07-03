import csv
import sys

# WSL-translated path to your Windows Downloads folder
input_file = "/mnt/c/Users/tosha/Downloads/ADA_1min.csv/ADA_1min.csv" 
output_file = "converted_ada_data.csv"

print("Converting ADA LOB snapshots into HFT execution stream...", file=sys.stderr)

with open(input_file, "r") as infile, open(output_file, "w") as outfile:
    reader = csv.reader(infile)
    header = next(reader)
    
    # Locate the midpoint reference index dynamically
    mid_idx = header.index("midpoint")
    order_id = 1
    
    for row in reader:
        if not row: continue
        mid_price = float(row[mid_idx])
        
        # Parse top 5 bid and ask levels from the snapshot rows
        for level in range(5):
            try:
                # Bids (Buy Side)
                bid_dist = float(row[header.index(f"bids_distance_{level}")])
                bid_vol = int(float(row[header.index(f"bids_limit_notional_{level}")]))
                bid_price = mid_price * (1.0 - bid_dist / 100.0)
                
                if bid_vol > 0:
                    outfile.write(f"NEW,ADA,{order_id},B,{bid_price:.2f},{bid_vol}\n")
                    order_id += 1
                
                # Asks (Sell Side)
                ask_dist = float(row[header.index(f"asks_distance_{level}")])
                ask_vol = int(float(row[header.index(f"asks_limit_notional_{level}")]))
                ask_price = mid_price * (1.0 + ask_dist / 100.0)
                
                if ask_vol > 0:
                    outfile.write(f"NEW,ADA,{order_id},S,{ask_price:.2f},{ask_vol}\n")
                    order_id += 1
            except (ValueError, IndexError):
                continue

print(f"Done! Saved converted stream to: {output_file}", file=sys.stderr)