#!/usr/bin/env python3
import matplotlib.pyplot as plt
import pandas as pd
import sys
import pathlib

input_file = sys.argv[1]

df = pd.read_csv(input_file)

graph_dir = 'send_graphs/'

pathlib.Path(graph_dir).mkdir(parents=True, exist_ok=True)

df['time-sec'] = df['time-ns'] / 1e9

df['packetsLostRate'] = df['packetsLost'].diff().fillna(0) / df['time-sec'].diff().fillna(1)

def plot_queue_delay(ax):
    ax.plot(df['time-sec'], df['queueDelay'], label='Queue Delay')
    ax.plot(df['time-sec'], df['queueDelayMax'], label='Queue Delay Max', linestyle='--')
    ax.plot(df['time-sec'], df['queueDelayMinAvg'], label='Queue Delay Min Avg', linestyle='--')
    ax.set_xlabel('Time (seconds)')
    ax.set_ylabel('Delay (seconds)')
    ax.set_title('Queue Delay over Time')
    ax.legend()
    ax.grid(True)

def plot_packets_loss_rate(ax):
    ax.plot(df['time-sec'], df['packetsLostRate'], label='Packet Loss Rate', color='green')
    ax.set_xlabel('Time (seconds)')
    ax.set_ylabel('Packet Loss Rate')
    ax.set_title('Packet Loss Rate over Time')
    ax.grid(True)
    
def plot_bitrates(ax):
    ax.plot(df['time-sec'], df['rateRtp'], label='Rate RTP', color='blue')
    ax.plot(df['time-sec'], df['rateTransmitted'], label='Rate Transmitted', color='orange')
    ax.plot(df['time-sec'], df['rateAcked'], label='Rate Acked', color='purple')
    ax.plot(df['time-sec'], df['targetBitrate'], label='Target Bitrate', color='brown')
    ax.set_xlabel('Time (seconds)')
    ax.set_ylabel('Bitrate (kbit/sec)')
    ax.set_title('Comparison of Bitrates over Time')
    ax.legend()
    ax.grid(True)

def plot_cwnd_srtt(ax):
    ax.plot(df['time-sec'], df['cwnd'], label='Congestion Window', color='tab:red')
    ax.set_xlabel('Time (seconds)')
    ax.set_ylabel('Congestion Window (Bytes)', color='tab:red')
    ax.tick_params(axis='y', labelcolor='tab:red')
    ax2 = ax.twinx()
    ax2.plot(df['time-sec'], df['sRtt'], label='Smoothed RTT', color='tab:blue')
    ax2.set_ylabel('SRTT (seconds)', color='tab:blue')
    ax2.tick_params(axis='y', labelcolor='tab:blue')
    ax.set_title('Congestion Window and SRTT over Time')

fig_queue_delay, ax_queue_delay = plt.subplots(figsize=(12, 6))
plot_queue_delay(ax_queue_delay)
fig_queue_delay.tight_layout()
fig_queue_delay.savefig(graph_dir + 'queue_delay_plot.png', dpi=300)

fig_packets_loss_rate, ax_packets_loss_rate = plt.subplots(figsize=(12, 6))
plot_packets_loss_rate(ax_packets_loss_rate)
fig_packets_loss_rate.tight_layout()
fig_packets_loss_rate.savefig(graph_dir + 'packets_loss_rate_plot.png', dpi=300)

fig_bitrate, ax_bitrate = plt.subplots(figsize=(12, 6))
plot_bitrates(ax_bitrate)
fig_bitrate.tight_layout()
fig_bitrate.savefig(graph_dir + 'bitrate_plot.png', dpi=300)

fig_cwnd_srtt, ax_cwnd_srtt = plt.subplots(figsize=(12, 6))
plot_cwnd_srtt(ax_cwnd_srtt)
fig_cwnd_srtt.tight_layout()
fig_cwnd_srtt.savefig(graph_dir + 'cwnd_srtt_plot.png', dpi=300)

fig_combined_vertical, axs_combined_vertical = plt.subplots(4, figsize=(12, 24))
plot_queue_delay(axs_combined_vertical[0])
plot_packets_loss_rate(axs_combined_vertical[1])
plot_bitrates(axs_combined_vertical[2])
plot_cwnd_srtt(axs_combined_vertical[3])
fig_combined_vertical.tight_layout()
fig_combined_vertical.subplots_adjust(hspace=0.5)
fig_combined_vertical.savefig(graph_dir + 'combined_plot_vertical.png', dpi=300)

fig_combined_grid, axs_combined_grid = plt.subplots(2, 2, figsize=(16, 12))
plot_queue_delay(axs_combined_grid[0, 0])
plot_packets_loss_rate(axs_combined_grid[0, 1])
plot_bitrates(axs_combined_grid[1, 0])
plot_cwnd_srtt(axs_combined_grid[1, 1])
fig_combined_grid.tight_layout()
fig_combined_grid.subplots_adjust(hspace=0.5, wspace=0.3)
fig_combined_grid.savefig(graph_dir + 'combined_plot_grid.png', dpi=300)
