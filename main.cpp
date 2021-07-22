#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unordered_map>
#include <string.h>
#include <vector>
#include <memory>
#include <iostream>
#include <unistd.h>
#include <chrono>
#include "tui.hpp"

using namespace std::chrono_literals;

#include "types.hpp"
#include "crc_32.hpp"

template <typename T>
using Ref = std::shared_ptr<T>;

struct MAC
{
    MAC_PARTS parts;

    uint64_t to_bytes() const
    {
        uint64_t bytes = 0;
        for (int i = 0; i < 6; i++)
        {
            bytes = bytes << 8;
            bytes |= parts[i];
        }
        return bytes;
    }

    std::string to_string() const
    {
        std::stringstream ss;
        ss << std::hex << std::setfill('0')
           << std::setw(2) << (uint64_t)parts[0] << ":"
           << std::setw(2) << (uint64_t)parts[1] << ":"
           << std::setw(2) << (uint64_t)parts[2] << ":"
           << std::setw(2) << (uint64_t)parts[3] << ":"
           << std::setw(2) << (uint64_t)parts[4] << ":"
           << std::setw(2) << (uint64_t)parts[5];
        return ss.str();
    }

    MAC(const std::string &str)
    {
        std::string m(str);
        std::replace(m.begin(), m.end(), ':', ' ');
        std::stringstream ss(m);
        ss >> std::hex >> parts[0] >> parts[1] >> parts[2] >> parts[3] >> parts[4] >> parts[5];
    }

    MAC(const MAC_PARTS &parts)
    {
        for (int i = 0; i < 6; i++)
            this->parts[i] = parts[i];
    }

    MAC(uint64_t bytes)
    {
        for (int i = 5; i >= 0; i--)
        {
            parts[i] = bytes & 0xFF;
            bytes >>= 8;
        }
    }

    MAC(const MAC &other)
    {
        memcpy(parts, other.parts, 6 * sizeof(uint16_t));
    }

    bool operator==(const MAC &other) const { return to_bytes() == other.to_bytes(); }
};

template <>
struct std::hash<MAC>
{
    size_t operator()(const MAC &mac) const
    {
        return std::hash<uint64_t>()(mac.to_bytes());
    }
};

struct Ether2Frame
{
    uint64_t
        dst : 48,
        src : 48,
        type : 16;

    //TODO: comentar uso de 1500 bytes (limitado pela linguagem (alternativa é usar a heap, mas perde fidedignidade))
    uint8_t data[1500];
    uint32_t CRC;

    Ether2Frame(const MAC &dst, const MAC &src, const char *const data, unsigned int data_size)
        : dst(dst.to_bytes()), src(src.to_bytes())
    {
        memcpy(this->data, data, data_size);
        CRC = CRC32(data, data_size);
    }
};

class EthernetPeer
{

protected:
    std::vector<Ref<EthernetPeer>> interfaces;

public:
    EthernetPeer(unsigned port_count) : interfaces(port_count) {}

    static void connect(const Ref<EthernetPeer> &A, const Ref<EthernetPeer> &B, unsigned portA, unsigned portB)
    {
        //Check if port already has a valid pointer
        if (A->interfaces[portA] == nullptr)
            A->interfaces[portA] = B;
        else if (A->interfaces[portA] != B)
            throw std::runtime_error("In A, portA is already connected to a different peer");

        if (B->interfaces[portB] == nullptr)
            B->interfaces[portB] = A;
        else if (B->interfaces[portB] != A)
            throw std::runtime_error("In B, portB is already connected to a different peer");

        //TODO: in case A and B are already connected, it will cause a reconnection (is this desirable?)

        A->interfaces[portA] = B;
        B->interfaces[portB] = A;
    }

    static void disconnect(const Ref<EthernetPeer> &A, const Ref<EthernetPeer> &B)
    {
        //Get in which port B is connected
        unsigned portA = 0;
        for (; portA < A->interfaces.size(); portA++)
            if (A->interfaces[portA] == B)
                break;

        //Get in which port a is connected
        unsigned portB = 0;
        for (; portB < B->interfaces.size(); portB++)
            if (B->interfaces[portB] == A)
                break;

        //If not found portA or portB, throw an exception
        if (portA == A->interfaces.size() || portB == B->interfaces.size())
            throw std::runtime_error("Peers are not connected");

        //Disconect peers
        A->interfaces[portA] = nullptr;
        B->interfaces[portB] = nullptr;
    }

    virtual void sendFrame(Ether2Frame &frame)
    {
        //Send frame to all ports with a valid peer
        for (unsigned int i = 0; i < interfaces.size(); i++)
            if (interfaces[i] != nullptr)
                interfaces[i]->receiveFrame(this, frame);
    }

    virtual void receiveFrame(const EthernetPeer *const sender_ptr, Ether2Frame &frame) = 0;
    virtual ~EthernetPeer() {}
};

class Host : public EthernetPeer
{
private:
    bool m_PromiscuousMode = false;

public:
    MAC m_MAC;
    virtual void receiveFrame(const EthernetPeer *const sender_ptr, Ether2Frame &frame) override
    {
        std::cout << "CurrentMAC: " << m_MAC.to_string() << std::endl;

        //Announce that this host has received the frame
        std::cout << "Received frame from " << MAC(frame.src).to_string() << ": " << frame.data << std::endl;
        std::cout << "Frame destination: " << MAC(frame.dst).to_string() << std::endl;

        //If the destination is not this host, drop the frame and return
        if (!m_PromiscuousMode && frame.dst != this->m_MAC.to_bytes())
        {
            std::cout << "Dropping frame" << std::endl;
            return;
        }

        std::cout << "Accepting frame" << std::endl;
    }

    Host(const MAC &mac, unsigned int port_count = 1) : EthernetPeer(port_count), m_MAC(mac) {}
};

struct SwitchTableEntry
{
    uint16_t interface;
    uint64_t lastUpdate;
};

const auto TTL = std::chrono::duration_cast<std::chrono::milliseconds>(10s).count();

using SwitchTable = std::unordered_map<MAC, SwitchTableEntry>;

class Switch : public EthernetPeer
{
private:
    const uint8_t MAX_TABLE_SIZE = 4;

    SwitchTable m_SwitchTable;

    void sendToAllExceptSender(uint16_t senderInterface, Ether2Frame &frame)
    {
        //Announce frame source and destination
        std::cout << "(SWITCH) Sending frame to all interfaces except " << senderInterface << std::endl;

        //Send frame to all ports with a valid peer
        for (unsigned int i = 0; i < interfaces.size(); i++)
            if (interfaces[i] != nullptr && i != senderInterface)
                interfaces[i]->receiveFrame(this, frame);
    }

    size_t getSenderInterface(const EthernetPeer *const sender_ptr)
    {
        //Gets the port index which matches with sender_ptr
        for (unsigned int i = 0; i < interfaces.size(); i++)
            if (interfaces[i] != nullptr && interfaces[i].get() == sender_ptr)
                return i;

        //If not found, throw an exception
        throw std::runtime_error("Sender not found");
    }

public:
    virtual void receiveFrame(const EthernetPeer *const sender_ptr, Ether2Frame &frame) override
    {
        //Announce frame receival
        std::cout << "(SWITCH) Received frame from " << MAC(frame.src).to_string() << ": " << frame.data << std::endl;
        std::cout << "(SWITCH) Frame destination: " << MAC(frame.dst).to_string() << std::endl;
        
        size_t senderInterface = 0;
        try
        {
            senderInterface = getSenderInterface(sender_ptr);
        }
        catch (const std::exception &e)
        {
            std::cerr << e.what() << '\n';
            return;
        }

        //Get current time in milliseconds
        uint64_t currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

        //TODO: check what should happen if the same MAC is presented in another interface before TTL expires
        //If sender not in switch table, add it, else update TTL and interface for MAC
        m_SwitchTable[MAC(frame.src)] = {(uint16_t)senderInterface, currentTime};

        auto findIt = m_SwitchTable.find(MAC(frame.dst));

        //If dest not in table or table is full, just send to all except sender
        if (findIt == m_SwitchTable.end() || m_SwitchTable.size() == MAX_TABLE_SIZE)
        {
            D(L("(SWITCH) Destination not in table, sending to all except sender"));
            sendToAllExceptSender(senderInterface, frame);
            return;
        }

        //If dest TTL expired, remove from switch table and send to all except sender
        if (currentTime - findIt->second.lastUpdate > TTL)
        {
            D(L("(SWITCH) TTL expired, removing from table and sending to all except sender"));
            m_SwitchTable.erase(frame.src);
            m_SwitchTable[MAC(frame.src)] = {(uint16_t)senderInterface, currentTime};
            sendToAllExceptSender(senderInterface, frame);
            return;
        }

        //If it's all ok, just send to destination
        D(L("(SWITCH) Sending to destination (it was in the switch table)"));
        this->interfaces[findIt->second.interface]->sendFrame(frame);
    }

    Switch(unsigned int port_count = 32) : EthernetPeer(port_count) {}
};

int main(int argc, char const *argv[])
{
    //Create two computers with a random MAC address
    Ref<Host> A = std::make_shared<Host>(MAC("AA:AA:AA:AA:AA:AA"));
    Ref<Host> B = std::make_shared<Host>(MAC("BB:BB:BB:BB:BB:BB"));
    Ref<Host> C = std::make_shared<Host>(MAC("CC:CC:CC:CC:CC:CC"));

    //Print A and B MAC addressess in string and byte form
    std::cout << "A: " << A->m_MAC.to_string() << std::endl;
    std::cout << "A (bytes): " << A->m_MAC.to_bytes() << std::endl;
    std::cout << "B: " << B->m_MAC.to_string() << std::endl;
    std::cout << "B (bytes): " << B->m_MAC.to_bytes() << std::endl;
    std::cout << std::endl
              << std::endl;

    //Create a switch with 2 ports
    Ref<Switch> S1 = std::make_shared<Switch>(2);
    Ref<Switch> S2 = std::make_shared<Switch>(3);

    //Connect A and B through switches
    EthernetPeer::connect(A, S1, 0, 1);
    EthernetPeer::connect(S1, S2, 0, 0);
    EthernetPeer::connect(B, S2, 0, 1);
    EthernetPeer::connect(C, S2, 0, 2);

    //TODO: change to a function

    //Create a frame from A to B with the message "Hello World"
    Ether2Frame frame(B->m_MAC, A->m_MAC, "Hello world!", 13);

    //Send the frame
    A->sendFrame(frame);

    return 0;
}
