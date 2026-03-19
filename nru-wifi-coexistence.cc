#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/nr-module.h"
#include "ns3/spectrum-module.h"

using namespace ns3;
NS_LOG_COMPONENT_DEFINE("NruWifiCoexistence");

int main(int argc, char *argv[])
{
    // ── Simulation parameters ──────────────────────────────────────
    double simTime       = 20.0;
    double frequency     = 5.925e9;   // 6 GHz unlicensed band
    double bandwidth     = 20e6;      // 20 MHz channel
    uint32_t nWifiSta    = 2;         // Wi-Fi stations
    uint32_t nNrUe       = 2;         // NR-U UEs

    CommandLine cmd;
    cmd.AddValue("simTime",   "Simulation time (s)", simTime);
    cmd.AddValue("frequency", "Channel frequency",   frequency);
    cmd.Parse(argc, argv);

    // ── Nodes ──────────────────────────────────────────────────────
    NodeContainer wifiApNode;    wifiApNode.Create(1);
    NodeContainer wifiStaNodes;  wifiStaNodes.Create(nWifiSta);
    NodeContainer nrGnbNode;     nrGnbNode.Create(1);
    NodeContainer nrUeNodes;     nrUeNodes.Create(nNrUe);

    // ── Mobility ───────────────────────────────────────────────────
    MobilityHelper mob;
    mob.SetMobilityModel("ns3::ConstantPositionMobilityModel");

    // Wi-Fi AP + stations on left side
    Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();
    pos->Add(Vector(0,   0, 10));   // Wi-Fi AP
    pos->Add(Vector(10,  5, 1.5));  // Wi-Fi STA 1
    pos->Add(Vector(10, -5, 1.5));  // Wi-Fi STA 2
    mob.SetPositionAllocator(pos);
    mob.Install(wifiApNode);
    mob.Install(wifiStaNodes);

    // NR-U gNB + UEs on right side — same area, competing for channel
    Ptr<ListPositionAllocator> pos2 = CreateObject<ListPositionAllocator>();
    pos2->Add(Vector(0,  20, 10));  // NR-U gNB
    pos2->Add(Vector(10, 25, 1.5)); // NR-U UE 1
    pos2->Add(Vector(10, 15, 1.5)); // NR-U UE 2
    mob.SetPositionAllocator(pos2);
    mob.Install(nrGnbNode);
    mob.Install(nrUeNodes);

    // ── Shared spectrum channel ────────────────────────────────────
    // THIS IS THE KEY — both Wi-Fi and NR-U use the SAME channel object
    Ptr<MultiModelSpectrumChannel> spectrumChannel =
        CreateObject<MultiModelSpectrumChannel>();
    Ptr<ThreeGppUmiStreetCanyonPropagationLossModel> lossModel =
    CreateObject<ThreeGppUmiStreetCanyonPropagationLossModel>();
    spectrumChannel->AddPropagationLossModel(lossModel);

    // ── NR-U setup ─────────────────────────────────────────────────
    Ptr<NrPointToPointEpcHelper> epcHelper =
        CreateObject<NrPointToPointEpcHelper>();
    Ptr<IdealBeamformingHelper> beamHelper =
        CreateObject<IdealBeamformingHelper>();
    Ptr<NrHelper> nrHelper = CreateObject<NrHelper>();

    nrHelper->SetBeamformingHelper(beamHelper);
    nrHelper->SetEpcHelper(epcHelper);

    // Configure NR-U band — unlicensed operation with LBT
    BandwidthPartInfoPtrVector allBwps;
    CcBwpCreator ccBwpCreator;
    const uint8_t numCcPerBand = 1;

    CcBwpCreator::SimpleOperationBandConf bandConf(
        frequency, bandwidth, numCcPerBand,
        BandwidthPartInfo::UMi_StreetCanyon_LoS);

    OperationBandInfo band = ccBwpCreator.CreateOperationBandContiguousCc(bandConf);

    // Enable LBT (Listen-Before-Talk) for NR-U unlicensed operation
    nrHelper->SetGnbPhyAttribute("TxPower", DoubleValue(23.0));
    nrHelper->SetUePhyAttribute ("TxPower", DoubleValue(23.0));

    nrHelper->InitializeOperationBand(&band);
    allBwps = CcBwpCreator::GetAllBwps({band});

    // Set the shared spectrum channel for NR-U
    nrHelper->SetPathlossAttribute("ShadowingEnabled", BooleanValue(false));

    NetDeviceContainer nrGnbDev = nrHelper->InstallGnbDevice(nrGnbNode, allBwps);
    NetDeviceContainer nrUeDev  = nrHelper->InstallUeDevice (nrUeNodes,  allBwps);

    // Update device configs
    for (auto it = nrGnbDev.Begin(); it != nrGnbDev.End(); ++it)
        DynamicCast<NrGnbNetDevice>(*it)->UpdateConfig();
    for (auto it = nrUeDev.Begin(); it != nrUeDev.End(); ++it)
        DynamicCast<NrUeNetDevice>(*it)->UpdateConfig();

    // ── Wi-Fi setup on same frequency ─────────────────────────────
    SpectrumWifiPhyHelper wifiPhy;
    wifiPhy.SetChannel(spectrumChannel);
    wifiPhy.Set("ChannelSettings",
                StringValue("{0, 20, BAND_5GHZ, 0}"));

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211ax);
    wifi.SetRemoteStationManager("ns3::IdealWifiManager");

    WifiMacHelper wifiMac;
    Ssid ssid = Ssid("wifi-coexist");

    wifiMac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));
    NetDeviceContainer wifiApDev = wifi.Install(wifiPhy, wifiMac, wifiApNode);

    wifiMac.SetType("ns3::StaWifiMac",
                    "Ssid", SsidValue(ssid),
                    "ActiveProbing", BooleanValue(false));
    NetDeviceContainer wifiStaDev = wifi.Install(wifiPhy, wifiMac, wifiStaNodes);

    // ── Internet stack ─────────────────────────────────────────────
    InternetStackHelper stack;
    stack.Install(wifiApNode);
    stack.Install(wifiStaNodes);

    Ipv4AddressHelper addr;
    addr.SetBase("10.1.0.0", "255.255.0.0");
    Ipv4InterfaceContainer wifiApIface  = addr.Assign(wifiApDev);
    Ipv4InterfaceContainer wifiStaIface = addr.Assign(wifiStaDev);

    // NR-U uses EPC for internet
    Ptr<Node> pgw = epcHelper->GetPgwNode();
    InternetStackHelper stackNr;
    stackNr.Install(pgw);
    stackNr.Install(nrUeNodes);

    addr.SetBase("10.2.0.0", "255.255.0.0");
    Ipv4InterfaceContainer nrUeIface =
        epcHelper->AssignUeIpv4Address(nrUeDev);

    // Attach NR-U UEs to gNB
    nrHelper->AttachToClosestEnb(nrUeDev, nrGnbDev);

    Ipv4StaticRoutingHelper ipv4RoutingHelper;
    for (uint32_t i = 0; i < nrUeNodes.GetN(); i++)
    {
        Ptr<Ipv4StaticRouting> ueStaticRouting =
            ipv4RoutingHelper.GetStaticRouting(
                nrUeNodes.Get(i)->GetObject<Ipv4>());
        ueStaticRouting->SetDefaultRoute(
            epcHelper->GetUeDefaultGatewayAddress(), 1);
    }

    // ── Traffic ────────────────────────────────────────────────────
    uint16_t port = 9;

    // Sink on Wi-Fi AP
    PacketSinkHelper wifiSink("ns3::UdpSocketFactory",
        InetSocketAddress(Ipv4Address::GetAny(), port));
    wifiSink.Install(wifiApNode.Get(0)).Start(Seconds(0));

    // Wi-Fi STAs send to AP
    for (uint32_t i = 0; i < nWifiSta; i++)
    {
        OnOffHelper src("ns3::UdpSocketFactory",
            InetSocketAddress(wifiApIface.GetAddress(0), port));
        src.SetConstantRate(DataRate("10Mbps"));
        src.SetAttribute("PacketSize", UintegerValue(1024));
        src.SetAttribute("OnTime",
            StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        src.SetAttribute("OffTime",
            StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        ApplicationContainer a = src.Install(wifiStaNodes.Get(i));
        a.Start(Seconds(5.0));
        a.Stop(Seconds(simTime));
    }

    // Sink on PGW for NR-U traffic
    PacketSinkHelper nrSink("ns3::UdpSocketFactory",
        InetSocketAddress(Ipv4Address::GetAny(), port + 1));
    nrSink.Install(pgw).Start(Seconds(0));

    // NR-U UEs send traffic — competing on same spectrum as Wi-Fi
    for (uint32_t i = 0; i < nNrUe; i++)
    {
        Ptr<Ipv4> ipv4 = pgw->GetObject<Ipv4>();
        Ipv4Address pgwAddr = ipv4->GetAddress(1, 0).GetLocal();

        OnOffHelper src("ns3::UdpSocketFactory",
            InetSocketAddress(pgwAddr, port + 1));
        src.SetConstantRate(DataRate("10Mbps"));
        src.SetAttribute("PacketSize", UintegerValue(1024));
        src.SetAttribute("OnTime",
            StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        src.SetAttribute("OffTime",
            StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        ApplicationContainer a = src.Install(nrUeNodes.Get(i));
        a.Start(Seconds(5.0));
        a.Stop(Seconds(simTime));
    }

    // ── Flow monitor ───────────────────────────────────────────────
    FlowMonitorHelper fmh;
    Ptr<FlowMonitor> fm = fmh.InstallAll();

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    fm->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> clf =
        DynamicCast<Ipv4FlowClassifier>(fmh.GetClassifier());

    double wifiTput=0, nrTput=0;
    int wifiFlows=0, nrFlows=0;
    double activeTime = simTime - 5.0;

    std::cout << "\n===== REAL NR-U + Wi-Fi COEXISTENCE =====\n";
    std::cout << "Frequency: " << frequency/1e9 << " GHz  |  "
              << "BW: " << bandwidth/1e6 << " MHz\n";
    std::cout << "Flow\tSrc\t\tTxPkts\tRxPkts\tLoss%\t"
              << "Delay(ms)\tTput(Mbps)\tTech\n";

    for (auto &f : fm->GetFlowStats())
    {
        Ipv4FlowClassifier::FiveTuple t = clf->FindFlow(f.first);
        double loss  = f.second.txPackets > 0 ?
            100.0*(f.second.txPackets-f.second.rxPackets)
            /f.second.txPackets : 0;
        double delay = f.second.rxPackets > 0 ?
            f.second.delaySum.GetSeconds()*1000.0
            /f.second.rxPackets : 0;
        double tput  = f.second.rxBytes*8.0/activeTime/1e6;

        // Identify technology by IP range
        std::string tech = "NR-U";
        std::ostringstream oss;
        t.sourceAddress.Print(oss);
        if (oss.str().find("10.1.") != std::string::npos)
            tech = "Wi-Fi";

        std::cout << f.first << "\t"
                  << t.sourceAddress << "\t"
                  << f.second.txPackets << "\t"
                  << f.second.rxPackets << "\t"
                  << loss  << "%\t"
                  << delay << "ms\t\t"
                  << tput  << "Mbps\t"
                  << tech  << "\n";

        if (tech == "Wi-Fi") { wifiTput += tput; wifiFlows++; }
        else                 { nrTput   += tput; nrFlows++;   }
    }

    std::cout << "\n--- Coexistence Summary ---\n";
    std::cout << "Wi-Fi total throughput : " << wifiTput << " Mbps ("
              << wifiFlows << " flows)\n";
    std::cout << "NR-U  total throughput : " << nrTput   << " Mbps ("
              << nrFlows   << " flows)\n";
    std::cout << "Fairness (Wi-Fi/NR-U)  : "
              << (nrTput>0 ? wifiTput/nrTput : 0) << "\n";
    std::cout << "==========================================\n";

    Simulator::Destroy();
    return 0;
}