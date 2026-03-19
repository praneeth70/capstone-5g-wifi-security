#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"

using namespace ns3;
NS_LOG_COMPONENT_DEFINE("CoexistenceBaseline");

int main(int argc, char *argv[])
{
    double simTime = 20.0;
    uint32_t nSta = 4;  // 2 Wi-Fi + 2 NR-U (all share same channel via EDCA/LBT)

    CommandLine cmd;
    cmd.AddValue("simTime", "Simulation time", simTime);
    cmd.Parse(argc, argv);

    NodeContainer apNode;   apNode.Create(1);
    NodeContainer staNodes; staNodes.Create(nSta);

    // Mobility
    MobilityHelper mob;
    mob.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();
    pos->Add(Vector(0,   0, 0));
    pos->Add(Vector(10,  0, 0));
    pos->Add(Vector(-10, 0, 0));
    pos->Add(Vector(0,  10, 0));
    pos->Add(Vector(0, -10, 0));
    mob.SetPositionAllocator(pos);
    mob.Install(apNode);
    mob.Install(staNodes);

    // Channel
    YansWifiChannelHelper channel = YansWifiChannelHelper::Default();
    YansWifiPhyHelper phy;
    phy.SetChannel(channel.Create());

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211n);
    wifi.SetRemoteStationManager("ns3::MinstrelHtWifiManager");

    Ssid ssid = Ssid("coexist");

    WifiMacHelper mac;
    // Install AP
    mac.SetType("ns3::ApWifiMac",
                "Ssid", SsidValue(ssid),
                "BeaconInterval", TimeValue(MicroSeconds(102400)));
    NetDeviceContainer apDev = wifi.Install(phy, mac, apNode);

    // Install STAs
    mac.SetType("ns3::StaWifiMac",
                "Ssid", SsidValue(ssid),
                "ActiveProbing", BooleanValue(false));
    NetDeviceContainer staDev = wifi.Install(phy, mac, staNodes);

    // Internet
    InternetStackHelper stack;
    stack.Install(apNode);
    stack.Install(staNodes);

    Ipv4AddressHelper addr;
    addr.SetBase("10.0.0.0", "255.255.255.0");
    Ipv4InterfaceContainer apIface  = addr.Assign(apDev);
    Ipv4InterfaceContainer staIface = addr.Assign(staDev);

    // Sink on AP — receives all traffic
    uint16_t port = 9;
    PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
        InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkApp = sinkHelper.Install(apNode.Get(0));
    sinkApp.Start(Seconds(0.0));
    sinkApp.Stop(Seconds(simTime));

    // Each STA sends UDP to AP — start at 5s to allow association
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

    std::cout << "\n===== BASELINE COEXISTENCE RESULTS =====\n";
    std::cout << "Flow\tSrc IP\t\tTxPkts\tRxPkts\tLoss%\t"
              << "Delay(ms)\tTput(Mbps)\n";

    for (auto &f : fm->GetFlowStats())
    {
        Ipv4FlowClassifier::FiveTuple t = clf->FindFlow(f.first);
        double loss  = f.second.txPackets > 0 ?
            100.0*(f.second.txPackets-f.second.rxPackets)/f.second.txPackets:0;
        double delay = f.second.rxPackets > 0 ?
            f.second.delaySum.GetSeconds()*1000.0/f.second.rxPackets : 0;
        double tput  = f.second.rxBytes*8.0/activeTime/1e6;

        std::cout << f.first   << "\t"
                  << t.sourceAddress << "\t"
                  << f.second.txPackets << "\t"
                  << f.second.rxPackets << "\t"
                  << loss  << "%\t"
                  << delay << "ms\t\t"
                  << tput  << "Mbps\n";

        totTx    += f.second.txPackets;
        totRx    += f.second.rxPackets;
        totDelay += delay;
        totTput  += tput;
        n++;
    }

    if (n > 0)
    {
        std::cout << "\n--- Summary (Baseline - No Attack) ---\n";
        std::cout << "Flows           : " << n << "\n";
        std::cout << "Total TX pkts   : " << totTx << "\n";
        std::cout << "Total RX pkts   : " << totRx << "\n";
        std::cout << "Overall loss    : "
                  << 100.0*(totTx-totRx)/totTx << "%\n";
        std::cout << "Avg delay       : " << totDelay/n << " ms\n";
        std::cout << "Total throughput: " << totTput << " Mbps\n";
        std::cout << "Per-node avg    : " << totTput/n << " Mbps\n";
        std::cout << "=========================================\n";
    }

    Simulator::Destroy();
    return 0;
}
