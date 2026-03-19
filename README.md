# Capstone: Security Vulnerabilities in 5G-WiFi Coexistence
Team 417 | VIT | Guide: Dr. Avuthu Avinash Reddy

## Simulations
| File | Description | Key Result |
|------|-------------|------------|
| coexistence-baseline.cc | Healthy coexistence | ~6 Mbps/node, 1.82ms delay |
| attack-lbt.cc | LBT channel flooding | Delay 92ms, loss 4.15% |

## Setup
1. Ubuntu 22.04 on VirtualBox (8GB RAM, 40GB disk, 4 cores)
2. Install ns-3.42 + 5G NR module (5g-lena-v3.1.y branch)
3. Copy .cc files into ~/ns-3-dev/scratch/
4. Run: ./ns3 run scratch/coexistence-baseline
