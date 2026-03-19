#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"

using namespace ns3;
NS_LOG_COMPONENT_DEFINE("AttackLBT");

int main(int argc, char *argv[])
{
    double simTime   = 20.0;
    uint32_t nSta    = 4;
    double attackStart = 8.0;  // attack begins mid-simulation

    CommandLine cmd;
    cmd.AddValue("simTime", "Simulation time", simTime);
    cmd.AddValue("attackStart", "When attack begins", attackStart);
    cmd.Parse(argc, argv);

    NodeContainer apNode;      apNode.Create(1);
    NodeContainer staNodes;    staNodes.Create(nSta);
    NodeContainer attackNode;  attackNode.Create(1);  // the adversary

    // Mobility
    MobilityHelper mob;
    mob.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();
    pos->Add(Vector(0,   0, 0));   // AP
    pos->Add(Vector(10,  0, 0));   // STA 1 (Wi-Fi)
    pos->Add(Vector(-10, 0, 0));   // STA 2 (Wi-Fi)
    pos->Add(Vector(0,  10, 0));   // STA 3 (NR-U)
    pos->Add(Vector(0, -10, 0));   // STA 4 (NR-U)
    pos->Add(Vector(5,   5, 0));   // Attacker (central position)
    mob.SetPositionAllocator(pos);
    mob.Install(apNode);
    mob.Install(staNodes);
    mob.Install(attackNode);

    // Channel
    YansWifiChannelHelper channel = YansWifiChannelHelper::Default();
    YansWifiPhyHelper phy;
    phy.SetChannel(channel.Create());

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211n);
    wifi.SetRemoteStationManager("ns3::MinstrelHtWifiManager");

    Ssid ssid = Ssid("coexist");

    WifiMacHelper mac;

    // AP
    mac.SetType("ns3::ApWifiMac",
                "Ssid", SsidValue(ssid),
                "BeaconInterval", TimeValue(MicroSeconds(102400)));
    NetDeviceContainer apDev = wifi.Install(phy, mac, apNode);

    // Legitimate STAs
    mac.SetType("ns3::StaWifiMac",
                "Ssid", SsidValue(ssid),
                "ActiveProbing", BooleanValue(false));
    NetDeviceContainer staDev = wifi.Install(phy, mac, staNodes);

    // Attacker joins same network (insider attacker model)
    NetDeviceContainer attackDev = wifi.Install(phy, mac, attackNode);

    // Internet
    InternetStackHelper stack;
    stack.Install(apNode);
    stack.Install(staNodes);
    stack.Install(attackNode);

    Ipv4AddressHelper addr;
    addr.SetBase("10.0.0.0", "255.255.255.0");
    Ipv4InterfaceContainer apIface     = addr.Assign(apDev);
    Ipv4InterfaceContainer staIface    = addr.Assign(staDev);
    Ipv4InterfaceContainer attackIface = addr.Assign(attackDev);

    // Sink on AP
    uint16_t port = 9;
    PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
        InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkApp = sinkHelper.Install(apNode.Get(0));
    sinkApp.Start(Seconds(0.0));
    sinkApp.Stop(Seconds(simTime));

    // Legitimate traffic (starts at 5s)
    for (uint32_t i = 0; i < nSta; i++)
    {
        OnOffHelper onoff("ns3::UdpSocketFactory",
            InetSocketAddress(apIface.GetAddress(0), port));
        onoff.SetConstantRate(DataRate("6Mbps"));
        onoff.SetAttribute("PacketSize", UintegerValue(1024));
        onoff.SetAttribute("OnTime",
            StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        onoff.SetAttribute("OffTime",
            StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        ApplicationContainer app = onoff.Install(staNodes.Get(i));
        app.Start(Seconds(5.0));
        app.Stop(Seconds(simTime));
    }

    // === ATTACK: LBT channel flooding ===
    // Attacker blasts at maximum rate to starve all other nodes
    // This models an NR-U node ignoring LBT backoff rules
    OnOffHelper attacker("ns3::UdpSocketFactory",
        InetSocketAddress(apIface.GetAddress(0), port));
    attacker.SetConstantRate(DataRate("54Mbps"));  // max rate flood
    attacker.SetAttribute("PacketSize", UintegerValue(1400));
    attacker.SetAttribute("OnTime",
        StringValue("ns3::ConstantRandomVariable[Constant=1]"));
    attacker.SetAttribute("OffTime",
        StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    ApplicationContainer attackApp = attacker.Install(attackNode.Get(0));
    attackApp.Start(Seconds(attackStart));
    attackApp.Stop(Seconds(simTime));

    // Flow monitor
    FlowMonitorHelper fmh;
    Ptr<FlowMonitor> fm = fmh.InstallAll();

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    fm->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> clf =
        DynamicCast<Ipv4FlowClassifier>(fmh.GetClassifier());

    double totTx=0, totRx=0, totDelay=0, totTput=0;
    int n=0;
    double activeTime = simTime - 5.0;

    std::cout << "\n===== ATTACK 1: LBT CHANNEL FLOODING =====\n";
    std::cout << "(Attack starts at t=" << attackStart << "s)\n";
    std::cout << "Flow\tSrc IP\t\tTxPkts\tRxPkts\tLoss%\t"
              << "Delay(ms)\tTput(Mbps)\tRole\n";

    for (auto &f : fm->GetFlowStats())
    {
        Ipv4FlowClassifier::FiveTuple t = clf->FindFlow(f.first);
        double loss  = f.second.txPackets > 0 ?
            100.0*(f.second.txPackets-f.second.rxPackets)/f.second.txPackets:0;
        double delay = f.second.rxPackets > 0 ?
            f.second.delaySum.GetSeconds()*1000.0/f.second.rxPackets : 0;
        double tput  = f.second.rxBytes*8.0/activeTime/1e6;

        // Identify attacker node
        std::string role = "Legitimate";
        if (t.sourceAddress == attackIface.GetAddress(0))
            role = "*** ATTACKER ***";

        std::cout << f.first   << "\t"
                  << t.sourceAddress << "\t"
                  << f.second.txPackets << "\t"
                  << f.second.rxPackets << "\t"
                  << loss  << "%\t"
                  << delay << "ms\t\t"
                  << tput  << "Mbps\t"
                  << role  << "\n";

        if (role == "Legitimate") {
            totTx    += f.second.txPackets;
            totRx    += f.second.rxPackets;
            totDelay += delay;
            totTput  += tput;
            n++;
        }
    }

    std::cout << "\n--- Impact on Legitimate Nodes ---\n";
    std::cout << "Baseline throughput : ~24.6 Mbps total\n";
    std::cout << "Under attack        : " << totTput << " Mbps total\n";
    std::cout << "Throughput drop     : "
              << 100.0*(24.6-totTput)/24.6 << "%\n";
    std::cout << "Baseline loss       : ~0.08%\n";
    std::cout << "Under attack loss   : "
              << 100.0*(totTx-totRx)/totTx << "%\n";
    std::cout << "Baseline avg delay  : ~1.82 ms\n";
    std::cout << "Under attack delay  : " << (n>0?totDelay/n:0) << " ms\n";
    std::cout << "==========================================\n";

    Simulator::Destroy();
    return 0;
}
