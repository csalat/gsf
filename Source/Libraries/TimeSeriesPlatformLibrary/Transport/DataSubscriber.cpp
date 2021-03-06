//******************************************************************************************************
//  DataSubscriber.cpp - Gbtc
//
//  Copyright � 2010, Grid Protection Alliance.  All Rights Reserved.
//
//  Licensed to the Grid Protection Alliance (GPA) under one or more contributor license agreements. See
//  the NOTICE file distributed with this work for additional information regarding copyright ownership.
//  The GPA licenses this file to you under the Eclipse Public License -v 1.0 (the "License"); you may
//  not use this file except in compliance with the License. You may obtain a copy of the License at:
//
//      http://www.opensource.org/licenses/eclipse-1.0.php
//
//  Unless agreed to in writing, the subject software distributed under the License is distributed on an
//  "AS-IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. Refer to the
//  License for the specific language governing permissions and limitations.
//
//  Code Modification History:
//  ----------------------------------------------------------------------------------------------------
//  03/26/2012 - Stephen C. Wills
//       Generated original version of source code.
//  03/22/2018 - J. Ritchie Carroll
//		 Updated DataSubscriber callback function signatures to always include instance reference.
//
//******************************************************************************************************

#include <sstream>
#include <boost/bind.hpp>

#include "DataSubscriber.h"
#include "Version.h"
#include "Constants.h"
#include "CompactMeasurementParser.h"
#include "../Common/Convert.h"

using namespace boost;
using namespace boost::asio;
using namespace boost::asio::ip;
using namespace GSF::TimeSeries;
using namespace GSF::TimeSeries::Transport;

// Convenience functions to perform simple conversions.
void WriteHandler(const ErrorCode& error, uint32_t bytesTransferred);

// --- SubscriptionInfo ---

SubscriptionInfo::SubscriptionInfo() :
    RemotelySynchronized(false),
    Throttled(false),
    UdpDataChannel(false),
    DataChannelLocalPort(9500),
    IncludeTime(true),
    LagTime(10.0),
    LeadTime(5.0),
    UseLocalClockAsRealTime(false),
    UseMillisecondResolution(false),
    ProcessingInterval(-1)
{
}

// --- SubscriberConnector ---

SubscriberConnector::SubscriberConnector() :
    m_errorMessageCallback(nullptr),
    m_reconnectCallback(nullptr),
    m_port(0),
    m_maxRetries(-1),
    m_retryInterval(2000),
    m_autoReconnect(true),
    m_cancel(false)
{
}

// Auto-reconnect handler.
void SubscriberConnector::AutoReconnect(DataSubscriber* subscriber)
{
    SubscriberConnector& connector = subscriber->GetSubscriberConnector();

    // Notify the user that we are attempting to reconnect.
    if (!connector.m_cancel && connector.m_errorMessageCallback != nullptr)
        connector.m_errorMessageCallback(subscriber, "Publisher connection terminated. Attempting to reconnect...");

    connector.Connect(*subscriber);

    // Notify the user that reconnect attempt was completed.
    if (!connector.m_cancel && connector.m_reconnectCallback != nullptr)
        connector.m_reconnectCallback(subscriber);
}

// Registers a callback to provide error messages each time
// the subscriber fails to connect during a connection sequence.
void SubscriberConnector::RegisterErrorMessageCallback(ErrorMessageCallback errorMessageCallback)
{
    m_errorMessageCallback = errorMessageCallback;
}

// Registers a callback to notify after an automatic reconnection attempt has been made.
void SubscriberConnector::RegisterReconnectCallback(ReconnectCallback reconnectCallback)
{
    m_reconnectCallback = reconnectCallback;
}

bool SubscriberConnector::Connect(DataSubscriber& subscriber, const SubscriptionInfo& info)
{
    subscriber.SetSubscriptionInfo(info);
    return Connect(subscriber);
}

// Begin connection sequence.
bool SubscriberConnector::Connect(DataSubscriber& subscriber)
{
    if (m_autoReconnect)
        subscriber.RegisterAutoReconnectCallback(&AutoReconnect);

    m_cancel = false;

    for (int i = 0; !m_cancel && (m_maxRetries == -1 || i < m_maxRetries); i++)
    {
        string errorMessage;
        bool connected = false;

        try
        {
            subscriber.Connect(m_hostname, m_port);
            connected = true;
            break;
        }
        catch (SubscriberException& ex)
        {
            errorMessage = ex.what();
        }
        catch (SystemError& ex)
        {
            errorMessage = ex.what();
        }
        catch (...)
        {
            errorMessage = current_exception_diagnostic_information(true);
        }

        if (!connected)
        {
            if (m_errorMessageCallback != nullptr)
            {
                stringstream errorMessageStream;
                errorMessageStream << "Failed to connect to \"" << m_hostname << ":" << m_port << "\": " << errorMessage;
                Thread(bind(m_errorMessageCallback, &subscriber, errorMessageStream.str()));
            }

            io_context io;
            deadline_timer timer(io, posix_time::milliseconds(m_retryInterval));
            timer.wait();
        }
    }

    return subscriber.IsConnected();
}

// Cancel all current and
// future connection sequences.
void SubscriberConnector::Cancel()
{
    m_cancel = true;
}

// Set the hostname of the publisher to connect to.
void SubscriberConnector::SetHostname(const string& hostname)
{
    m_hostname = hostname;
}

// Set the port that the publisher is listening on.
void SubscriberConnector::SetPort(uint16_t port)
{
    m_port = port;
}

// Set the maximum number of retries during a connection sequence.
void SubscriberConnector::SetMaxRetries(int32_t maxRetries)
{
    m_maxRetries = maxRetries;
}

// Set the interval of idle time (in milliseconds) between connection attempts.
void SubscriberConnector::SetRetryInterval(int32_t retryInterval)
{
    m_retryInterval = retryInterval;
}

// Set the flag that determines whether the subscriber should
// automatically attempt to reconnect when the connection is terminated.
void SubscriberConnector::SetAutoReconnect(bool autoReconnect)
{
    m_autoReconnect = autoReconnect;
}

// Gets the hostname of the publisher to connect to.
string SubscriberConnector::GetHostname() const
{
    return m_hostname;
}

// Gets the port that the publisher is listening on.
uint16_t SubscriberConnector::GetPort() const
{
    return m_port;
}

// Gets the maximum number of retries during a connection sequence.
int32_t SubscriberConnector::GetMaxRetries() const
{
    return m_maxRetries;
}

// Gets the interval of idle time between connection attempts.
int32_t SubscriberConnector::GetRetryInterval() const
{
    return m_retryInterval;
}

// Gets the flag that determines whether the subscriber should
// automatically attempt to reconnect when the connection is terminated.
bool SubscriberConnector::GetAutoReconnect() const
{
    return m_autoReconnect;
}

// --- DataSubscriber ---

DataSubscriber::DataSubscriber() :  // NOLINT
    m_compressPayloadData(true),
    m_compressMetadata(true),
    m_compressSignalIndexCache(true),
    m_disconnecting(false),
    m_userData(nullptr),
    m_totalCommandChannelBytesReceived(0UL),
    m_totalDataChannelBytesReceived(0UL),
    m_totalMeasurementsReceived(0UL),
    m_connected(false),
    m_subscribed(false),
    m_timeIndex(0),
    m_tsscResetRequested(false),
    m_tsscSequenceNumber(0),
    m_commandChannelSocket(m_commandChannelService),
    m_readBuffer(Common::MaxPacketSize),
    m_writeBuffer(Common::MaxPacketSize),
    m_dataChannelSocket(m_dataChannelService),
    m_statusMessageCallback(nullptr),
    m_errorMessageCallback(nullptr),
    m_dataStartTimeCallback(nullptr),
    m_metadataCallback(nullptr),
    m_newMeasurementsCallback(nullptr),
    m_processingCompleteCallback(nullptr),
    m_configurationChangedCallback(nullptr),
    m_connectionTerminatedCallback(nullptr),
    m_autoReconnectCallback(nullptr)
{
    m_baseTimeOffsets[0] = 0;
    m_baseTimeOffsets[1] = 0;
}

// Destructor calls disconnect to clean up after itself.
DataSubscriber::~DataSubscriber()
{
    Disconnect();
}

DataSubscriber::CallbackDispatcher::CallbackDispatcher() :
    Source(nullptr),
    Data(nullptr),
    Function(nullptr)
{
}

// All callbacks are run from the callback thread from here.
void DataSubscriber::RunCallbackThread()
{
    while (true)
    {
        m_callbackQueue.WaitForData();

        if (m_disconnecting)
            break;

        const CallbackDispatcher dispatcher = m_callbackQueue.Dequeue();
        dispatcher.Function(dispatcher.Source, *dispatcher.Data);
    }
}

// All responses received from the server are handled by this thread with the
// exception of data packets which may or may not be handled by this thread.
void DataSubscriber::RunCommandChannelResponseThread()
{
    async_read(m_commandChannelSocket, buffer(m_readBuffer, Common::PayloadHeaderSize), bind(&DataSubscriber::ReadPayloadHeader, this, _1, _2));
    m_commandChannelService.run();
}

// Callback for async read of the payload header.
void DataSubscriber::ReadPayloadHeader(const ErrorCode& error, uint32_t bytesTransferred)
{
    const uint32_t PacketSizeOffset = 4;

    if (m_disconnecting)
        return;

    if (error == error::connection_aborted || error == error::connection_reset || error == error::eof)
    {
        // Connection closed by peer; terminate connection
        Thread(bind(&DataSubscriber::ConnectionTerminatedDispatcher, this));
        return;
    }

    if (error)
    {
        stringstream errorMessageStream;

        errorMessageStream << "Error reading data from command channel: ";
        errorMessageStream << SystemError(error).what();

        DispatchErrorMessage(errorMessageStream.str());
        return;
    }

    // Gather statistics
    m_totalCommandChannelBytesReceived += Common::PayloadHeaderSize;

    const uint32_t packetSize = m_endianConverter.ConvertLittleEndian(*reinterpret_cast<uint32_t*>(&m_readBuffer[PacketSizeOffset]));

    if (packetSize > static_cast<uint32_t>(m_readBuffer.size()))
        m_readBuffer.resize(packetSize);

    // Read packet (payload body)
    // This read method is guaranteed not to return until the
    // requested size has been read or an error has occurred.
    async_read(m_commandChannelSocket, buffer(m_readBuffer, packetSize), bind(&DataSubscriber::ReadPacket, this, _1, _2));
}

// Callback for async read of packets.
void DataSubscriber::ReadPacket(const ErrorCode& error, uint32_t bytesTransferred)
{
    if (m_disconnecting)
        return;

    if (error == error::connection_aborted || error == error::connection_reset || error == error::eof)
    {
        // Connection closed by peer; terminate connection
        Thread(bind(&DataSubscriber::ConnectionTerminatedDispatcher, this));
        return;
    }

    if (error)
    {
        stringstream errorMessageStream;

        errorMessageStream << "Error reading data from command channel: ";
        errorMessageStream << SystemError(error).what();

        DispatchErrorMessage(errorMessageStream.str());
        return;
    }

    // Gather statistics
    m_totalCommandChannelBytesReceived += bytesTransferred;

    // Process response
    ProcessServerResponse(&m_readBuffer[0], 0, bytesTransferred);

    // Read next payload header
    async_read(m_commandChannelSocket, buffer(m_readBuffer, Common::PayloadHeaderSize), bind(&DataSubscriber::ReadPayloadHeader, this, _1, _2));
}

// If the user defines a separate UDP channel for their
// subscription, data packets get handled from this thread.
void DataSubscriber::RunDataChannelResponseThread()
{
    vector<uint8_t> buffer(Common::MaxPacketSize);
    uint32_t length;

    udp::endpoint endpoint(m_hostAddress, 0);
    ErrorCode error;
    stringstream errorMessageStream;

    while (true)
    {
        length = m_dataChannelSocket.receive_from(asio::buffer(buffer), endpoint, 0, error);

        if (m_disconnecting)
            break;

        if (error)
        {
            errorMessageStream << "Error reading data from command channel: ";
            errorMessageStream << SystemError(error).what();
            DispatchErrorMessage(errorMessageStream.str());
            break;
        }

        // Gather statistics
        m_totalDataChannelBytesReceived += length;

        ProcessServerResponse(&buffer[0], 0, length);
    }
}

// Handles success messages received from the server.
void DataSubscriber::HandleSucceeded(uint8_t commandCode, uint8_t* data, uint32_t offset, uint32_t length)
{
    const uint32_t messageLength = length / sizeof(char);
    stringstream messageStream;

    char* messageStart;
    char* messageEnd;
    char* messageIter;

    switch (commandCode)
    {
        case ServerCommand::MetadataRefresh:
            // Metadata refresh message is not sent with a
            // message, but rather the metadata itself.
            HandleMetadataRefresh(data, offset, length);
            break;

        case ServerCommand::Subscribe:
        case ServerCommand::Unsubscribe:
            // Do not break on these messages because there is
            // still an associated message to be processed.
            m_subscribed = (commandCode == ServerCommand::Subscribe);

        case ServerCommand::Authenticate:
        case ServerCommand::RotateCipherKeys:
            // Each of these responses come with a message that will
            // be delivered to the user via the status message callback.
            if (data != nullptr)
            {
                messageStart = reinterpret_cast<char*>(data + offset);
                messageEnd = messageStart + messageLength;
                messageStream << "Received success code in response to server command 0x" << hex << static_cast<int>(commandCode) << ": ";

                for (messageIter = messageStart; messageIter < messageEnd; ++messageIter)
                    messageStream << *messageIter;

                DispatchStatusMessage(messageStream.str());
            }
            break;

        default:
            // If we don't know what the message is, we can't interpret
            // the data sent with the packet. Deliver an error message
            // to the user via the error message callback.
            messageStream << "Received success code in response to unknown server command 0x" << hex << static_cast<int>(commandCode);
            DispatchErrorMessage(messageStream.str());
            break;
    }
}

// Handles failure messages from the server.
void DataSubscriber::HandleFailed(uint8_t commandCode, uint8_t* data, uint32_t offset, uint32_t length)
{
    if (data == nullptr)
        return;

    const uint32_t messageLength = length / sizeof(char);
    stringstream messageStream;

    char* messageStart;
    char* messageEnd;
    char* messageIter;

    messageStart = reinterpret_cast<char*>(data + offset);
    messageEnd = messageStart + messageLength;
    messageStream << "Received failure code from server command 0x" << hex << static_cast<int>(commandCode) << ": ";

    for (messageIter = messageStart; messageIter < messageEnd; ++messageIter)
        messageStream << *messageIter;

    DispatchErrorMessage(messageStream.str());
}

// Handles metadata refresh messages from the server.
void DataSubscriber::HandleMetadataRefresh(uint8_t* data, uint32_t offset, uint32_t length)
{
    Dispatch(&MetadataDispatcher, data, offset, length);
}

// Handles data packets from the server.
void DataSubscriber::HandleDataPacket(uint8_t* data, uint32_t offset, uint32_t length)
{
    Dispatch(&NewMeasurementsDispatcher, data, offset, length);
}

// Handles data start time reported by the server at the beginning of a subscription.
void DataSubscriber::HandleDataStartTime(uint8_t* data, uint32_t offset, uint32_t length)
{
    Dispatch(&DataStartTimeDispatcher, data, offset, length);
}

// Handles processing complete message sent by the server at the end of a temporal session.
void DataSubscriber::HandleProcessingComplete(uint8_t* data, uint32_t offset, uint32_t length)
{
    Dispatch(&ProcessingCompleteDispatcher, data, offset, length);
}

// Cache signal IDs sent by the server into the signal index cache.
void DataSubscriber::HandleUpdateSignalIndexCache(uint8_t* data, uint32_t offset, uint32_t length)
{
    if (data == nullptr)
        return;

    const bool swapBytes = m_endianConverter.NativeOrder() == EndianConverter::LittleEndian;

    uint32_t* referenceCountPtr;
    uint32_t referenceCount;

    uint16_t* signalIndexPtr;
    uint8_t* signalIDPtr;
    uint32_t* sourceSizePtr;
    char* sourcePtr;
    uint32_t* idPtr;

    uint16_t signalIndex;
    Guid signalID;
    uint32_t sourceSize;
    string source;
    uint32_t id;

    vector<uint8_t> uncompressed;
    stringstream sourceStream;
    char* sourceIter;

    if (m_compressSignalIndexCache)
    {
        const MemoryStream payloadStream(data, offset, length);

        // Perform zlib decompression on buffer
        Decompressor decompressor;

        decompressor.push(GZipStream());
        decompressor.push(payloadStream);

        CopyStream(decompressor, uncompressed);
    }
    else
    {
        for (uint32_t i = offset; i < length; i++)
            uncompressed.push_back(data[i]);
    }

    // Begin by emptying the cache
    m_signalIndexCache.Clear();

    // Skip 4-byte length and 16-byte subscriber ID
    // We may need to parse these in the future...
    referenceCountPtr = reinterpret_cast<uint32_t*>(uncompressed.data() + 20);
    referenceCount = m_endianConverter.ConvertBigEndian(*referenceCountPtr);

    // Set up signalIndexPtr before entering the loop
    signalIndexPtr = reinterpret_cast<uint16_t*>(referenceCountPtr + 1);

    for (uint32_t i = 0; i < referenceCount; ++i)
    {
        // Begin setting up pointers
        signalIDPtr = reinterpret_cast<uint8_t*>(signalIndexPtr + 1);
        sourceSizePtr = reinterpret_cast<uint32_t*>(signalIDPtr + 16);

        // Get the source size now so we can use it to find the ID
        sourceSize = static_cast<uint32_t>(m_endianConverter.ConvertBigEndian(*sourceSizePtr)) / sizeof(char);

        // Continue setting up pointers
        sourcePtr = reinterpret_cast<char*>(sourceSizePtr + 1);
        idPtr = reinterpret_cast<uint32_t*>(sourcePtr + sourceSize);

        // Build string from binary data
        for (sourceIter = sourcePtr; sourceIter < sourcePtr + sourceSize; ++sourceIter)
            sourceStream << *sourceIter;

        // Set values for measurement key
        signalIndex = m_endianConverter.ConvertBigEndian(*signalIndexPtr);
        signalID = ToGuid(signalIDPtr, swapBytes);
        source = sourceStream.str();
        id = m_endianConverter.ConvertBigEndian(*idPtr);

        // Add measurement key to the cache
        m_signalIndexCache.AddMeasurementKey(signalIndex, signalID, source, id);

        // Advance signalIndexPtr to the next signal
        // index and clear out the string stream
        signalIndexPtr = reinterpret_cast<uint16_t*>(idPtr + 1);
        sourceStream.str("");
    }

    // There is additional data about unauthorized signal
    // IDs that may need to be parsed in the future...
}

// Updates base time offsets.
void DataSubscriber::HandleUpdateBaseTimes(uint8_t* data, uint32_t offset, uint32_t length)
{
    if (data == nullptr)
        return;

    int32_t* timeIndexPtr = reinterpret_cast<int32_t*>(data + offset);
    int64_t* timeOffsetsPtr = reinterpret_cast<int64_t*>(timeIndexPtr + 1);

    m_timeIndex = m_endianConverter.ConvertBigEndian(*timeIndexPtr);
    m_baseTimeOffsets[0] = m_endianConverter.ConvertBigEndian(timeOffsetsPtr[0]);
    m_baseTimeOffsets[1] = m_endianConverter.ConvertBigEndian(timeOffsetsPtr[1]);
}

// Handles configuration changed message sent by the server at the end of a temporal session.
void DataSubscriber::HandleConfigurationChanged(uint8_t* data, uint32_t offset, uint32_t length)
{
    Dispatch(&ConfigurationChangedDispatcher);
}

// Dispatches the given function to the callback thread.
void DataSubscriber::Dispatch(DispatcherFunction function)
{
    Dispatch(function, nullptr, 0, 0);
}

// Dispatches the given function to the callback thread and provides the given data to that function when it is called.
void DataSubscriber::Dispatch(DispatcherFunction function, const uint8_t* data, uint32_t offset, uint32_t length)
{
    CallbackDispatcher dispatcher;
    SharedPtr<vector<uint8_t>> dataVector = NewSharedPtr<vector<uint8_t>>();

    dataVector->resize(length);

    if (data != nullptr)
    {
        for (uint32_t i = 0; i < length; ++i)
            dataVector->at(i) = data[offset + i];
    }

    dispatcher.Source = this;
    dispatcher.Data = dataVector;
    dispatcher.Function = function;

    m_callbackQueue.Enqueue(dispatcher);
}

// Invokes the status message callback on the callback thread and provides the given message to it.
void DataSubscriber::DispatchStatusMessage(const string& message)
{
    const uint32_t messageSize = message.size() * sizeof(char);
    const char* data = message.c_str();

    Dispatch(&StatusMessageDispatcher, reinterpret_cast<const uint8_t*>(data), 0, messageSize);
}

// Invokes the error message callback on the callback thread and provides the given message to it.
void DataSubscriber::DispatchErrorMessage(const string& message)
{
    const uint32_t messageSize = message.size() * sizeof(char);
    const char* data = message.c_str();

    Dispatch(&ErrorMessageDispatcher, reinterpret_cast<const uint8_t*>(data), 0, messageSize);
}

// Dispatcher function for status messages. Decodes the message and provides it to the user via the status message callback.
void DataSubscriber::StatusMessageDispatcher(DataSubscriber* source, const vector<uint8_t>& buffer)
{
    if (source == nullptr)
        return;
    
    const MessageCallback statusMessageCallback = source->m_statusMessageCallback;
    
    if (statusMessageCallback != nullptr)
    {
        stringstream messageStream;

        for (unsigned char i : buffer)
            messageStream << i;

        statusMessageCallback(source, messageStream.str());
    }
}

// Dispatcher function for error messages. Decodes the message and provides it to the user via the error message callback.
void DataSubscriber::ErrorMessageDispatcher(DataSubscriber* source, const vector<uint8_t>& buffer)
{
    if (source == nullptr)
        return;

    const MessageCallback errorMessageCallback = source->m_errorMessageCallback;

    if (errorMessageCallback != nullptr)
    {
        stringstream messageStream;

        for (unsigned char i : buffer)
            messageStream << i;

        errorMessageCallback(source, messageStream.str());
    }
}

// Dispatcher function for data start time. Decodes the start time and provides it to the user via the data start time callback.
void DataSubscriber::DataStartTimeDispatcher(DataSubscriber* source, const vector<uint8_t>& buffer)
{
    if (source == nullptr)
        return;

    const DataStartTimeCallback dataStartTimeCallback = source->m_dataStartTimeCallback;

    if (dataStartTimeCallback != nullptr)
    {
        EndianConverter endianConverter = source->m_endianConverter;
        const int64_t dataStartTime = endianConverter.ConvertBigEndian(*reinterpret_cast<const int64_t*>(&buffer[0]));
        dataStartTimeCallback(source, dataStartTime);
    }
}

// Dispatcher function for metadata. Provides encoded metadata to the user via the metadata callback.
void DataSubscriber::MetadataDispatcher(DataSubscriber* source, const vector<uint8_t>& buffer)
{
    if (source == nullptr)
        return;

    const MetadataCallback metadataCallback = source->m_metadataCallback;

    if (metadataCallback != nullptr)
        metadataCallback(source, buffer);
}

// Dispatcher function for new measurements. Decodes the measurements and provides them to the user via the new measurements callback.
void DataSubscriber::NewMeasurementsDispatcher(DataSubscriber* source, const vector<uint8_t>& buffer)
{
    if (source == nullptr)
        return;

    const NewMeasurementsCallback newMeasurementsCallback = source->m_newMeasurementsCallback;

    if (newMeasurementsCallback != nullptr)
    {
        SubscriptionInfo& info = source->m_subscriptionInfo;
        uint8_t dataPacketFlags;
        int64_t frameLevelTimestamp = -1;
        uint32_t offset = 0;

        bool includeTime = info.IncludeTime;

        // Read data packet flags
        dataPacketFlags = buffer[offset];
        offset++;

        // Read frame-level timestamp, if available
        if (dataPacketFlags & DataPacketFlags::Synchronized)
        {
            frameLevelTimestamp = source->m_endianConverter.ConvertBigEndian(*reinterpret_cast<const int64_t*>(&buffer[offset]));
            offset += 8;
            includeTime = false;
        }

        // Read measurement count and gather statistics
        const uint32_t count = source->m_endianConverter.ConvertBigEndian(*reinterpret_cast<const uint32_t*>(&buffer[offset]));
        source->m_totalMeasurementsReceived += count;
        offset += 4;

        vector<MeasurementPtr> measurements;

        if (dataPacketFlags & DataPacketFlags::Compressed)
            ParseTSSCMeasurements(source, buffer, offset, measurements);
        else
            ParseCompactMeasurements(source, buffer, offset, includeTime, info.UseMillisecondResolution, frameLevelTimestamp, measurements);

        newMeasurementsCallback(source, measurements);
    }
}

void DataSubscriber::ParseTSSCMeasurements(DataSubscriber* source, const vector<uint8_t>& buffer, uint32_t offset, vector<MeasurementPtr>& measurements)
{
    if (source == nullptr)
        return;

    MeasurementPtr measurement;
    string errorMessage;

    if (buffer[offset] != 85)
    {
        stringstream errorMessageStream;

        errorMessageStream << "TSSC version not recognized: 0x";
        errorMessageStream << hex << static_cast<int>(buffer[offset]);

        throw SubscriberException(errorMessageStream.str());
    }
    offset++;

    const uint16_t sequenceNumber = source->m_endianConverter.ConvertBigEndian(*reinterpret_cast<const uint16_t*>(&buffer[offset]));
    offset += 2;

    if (sequenceNumber == 0 && source->m_tsscSequenceNumber > 0)
    {
        if (!source->m_tsscResetRequested)
        {
            stringstream statusMessageStream;
            statusMessageStream << "TSSC algorithm reset before sequence number: ";
            statusMessageStream << source->m_tsscSequenceNumber;
            source->DispatchStatusMessage(statusMessageStream.str());
        }

        source->m_tsscMeasurementParser.Reset();
        source->m_tsscSequenceNumber = 0;
        source->m_tsscResetRequested = false;
    }

    if (source->m_tsscSequenceNumber != sequenceNumber)
    {
        if (!source->m_tsscResetRequested)
        {
            stringstream errorMessageStream;
            errorMessageStream << "TSSC is out of sequence. Expecting: ";
            errorMessageStream << source->m_tsscSequenceNumber;
            errorMessageStream << ", Received: ";
            errorMessageStream << sequenceNumber;
            source->DispatchErrorMessage(errorMessageStream.str());
        }

        // Ignore packets until the reset has occurred.
        return;
    }

    try
    {
        source->m_tsscMeasurementParser.SetBuffer(buffer, offset);

        Guid signalID;
        string measurementSource;
        uint32_t measurementID;
        uint16_t id;
        int64_t time;
        uint32_t quality;
        float32_t value;

        while (source->m_tsscMeasurementParser.TryGetMeasurement(id, time, quality, value))
        {
            if (source->m_signalIndexCache.GetMeasurementKey(id, signalID, measurementSource, measurementID))
            {
                measurement = NewSharedPtr<Measurement>();

                measurement->SignalID = signalID;
                measurement->Source = measurementSource;
                measurement->ID = measurementID;
                measurement->Timestamp = time;
                measurement->Flags = quality;
                measurement->Value = value;

                measurements.push_back(measurement);
            }
        }
    }
    catch (SubscriberException& ex)
    {
        errorMessage = ex.what();
    }
    catch (...)
    {
        errorMessage = current_exception_diagnostic_information(true);
    }

    if (errorMessage.length() > 0)
    {
        stringstream errorMessageStream;
        errorMessageStream << "Decompression failure: ";
        errorMessageStream << errorMessage;
        source->DispatchErrorMessage(errorMessageStream.str());
    }

    source->m_tsscSequenceNumber++;

    // Do not increment to 0 on roll-over
    if (source->m_tsscSequenceNumber == 0)
        source->m_tsscSequenceNumber = 1;
}

void DataSubscriber::ParseCompactMeasurements(DataSubscriber* source, const vector<uint8_t>& buffer, uint32_t offset, bool includeTime, bool useMillisecondResolution, int64_t frameLevelTimestamp, vector<MeasurementPtr>& measurements)
{
    if (source == nullptr)
        return;

    const MessageCallback errorMessageCallback = source->m_errorMessageCallback;

    MeasurementPtr measurement;

    // Create measurement parser
    CompactMeasurementParser measurementParser(source->m_signalIndexCache, source->m_baseTimeOffsets, includeTime, useMillisecondResolution);

    uint32_t length = buffer.size() - offset;

    while (length > 0)
    {
        if (!measurementParser.TryParseMeasurement(buffer, offset, length))
        {
            if (errorMessageCallback != nullptr)
                errorMessageCallback(source, "Error parsing measurement");

            break;
        }

        measurement = measurementParser.GetParsedMeasurement();

        if (frameLevelTimestamp > -1)
            measurement->Timestamp = frameLevelTimestamp;

        measurements.push_back(measurement);
    }
}

// Dispatcher for processing complete message that is sent by the server at the end of a temporal session.
void DataSubscriber::ProcessingCompleteDispatcher(DataSubscriber* source, const vector<uint8_t>& buffer)
{
    if (source == nullptr)
        return;

    const MessageCallback processingCompleteCallback = source->m_processingCompleteCallback;

    if (processingCompleteCallback != nullptr)
    {
        stringstream messageStream;

        for (uint32_t i = 0; i < buffer.size(); ++i)
            messageStream << buffer[i];

        processingCompleteCallback(source, messageStream.str());
    }
}

// Dispatcher for processing complete message that is sent by the server at the end of a temporal session.
void DataSubscriber::ConfigurationChangedDispatcher(DataSubscriber* source, const vector<uint8_t>& buffer)
{
    if (source == nullptr)
        return;

    const ConfigurationChangedCallback configurationChangedCallback = source->m_configurationChangedCallback;

    if (configurationChangedCallback != nullptr)
        source->m_configurationChangedCallback(source);
}

// Dispatcher for connection terminated. This is called from its own separate thread
// in order to cleanly shut down the subscriber in case the connection was terminated
// by the peer. Additionally, this allows the user to automatically reconnect in their
// callback function without having to spawn their own separate thread.
void DataSubscriber::ConnectionTerminatedDispatcher()
{
    Disconnect(true);
}

// Processes a response sent by the server. Response codes are defined in the header file "Constants.h".
void DataSubscriber::ProcessServerResponse(uint8_t* buffer, uint32_t offset, uint32_t length)
{
    const uint32_t PacketHeaderSize = 6;

    uint8_t* packetBodyStart = buffer + PacketHeaderSize;
    const uint32_t packetBodyLength = length - PacketHeaderSize;

    const uint8_t responseCode = buffer[0];
    const uint8_t commandCode = buffer[1];

    switch (responseCode)
    {
        case ServerResponse::Succeeded:
            HandleSucceeded(commandCode, packetBodyStart, 0, packetBodyLength);
            break;

        case ServerResponse::Failed:
            HandleFailed(commandCode, packetBodyStart, 0, packetBodyLength);
            break;

        case ServerResponse::DataPacket:
            HandleDataPacket(packetBodyStart, 0, packetBodyLength);
            break;

        case ServerResponse::DataStartTime:
            HandleDataStartTime(packetBodyStart, 0, packetBodyLength);
            break;

        case ServerResponse::ProcessingComplete:
            HandleProcessingComplete(packetBodyStart, 0, packetBodyLength);
            break;

        case ServerResponse::UpdateSignalIndexCache:
            HandleUpdateSignalIndexCache(packetBodyStart, 0, packetBodyLength);
            break;

        case ServerResponse::UpdateBaseTimes:
            HandleUpdateBaseTimes(packetBodyStart, 0, packetBodyLength);
            break;

        case ServerResponse::ConfigurationChanged:
            HandleConfigurationChanged(packetBodyStart, 0, packetBodyLength);
            break;

        case ServerResponse::NoOP:
            break;

        default:
            stringstream errorMessageStream;
            errorMessageStream << "Encountered unexpected server response code: 0x";
            errorMessageStream << hex << static_cast<int>(responseCode);
            DispatchErrorMessage(errorMessageStream.str());
            break;
    }
}

// Registers the status message callback.
void DataSubscriber::RegisterStatusMessageCallback(MessageCallback statusMessageCallback)
{
    m_statusMessageCallback = statusMessageCallback;
}

// Registers the error message callback.
void DataSubscriber::RegisterErrorMessageCallback(MessageCallback errorMessageCallback)
{
    m_errorMessageCallback = errorMessageCallback;
}

// Registers the data start time callback.
void DataSubscriber::RegisterDataStartTimeCallback(DataStartTimeCallback dataStartTimeCallback)
{
    m_dataStartTimeCallback = dataStartTimeCallback;
}

// Registers the metadata callback.
void DataSubscriber::RegisterMetadataCallback(MetadataCallback metadataCallback)
{
    m_metadataCallback = metadataCallback;
}

// Registers the new measurements callback.
void DataSubscriber::RegisterNewMeasurementsCallback(NewMeasurementsCallback newMeasurementsCallback)
{
    m_newMeasurementsCallback = newMeasurementsCallback;
}

// Registers the processing complete callback.
void DataSubscriber::RegisterProcessingCompleteCallback(MessageCallback processingCompleteCallback)
{
    m_processingCompleteCallback = processingCompleteCallback;
}

// Registers the configuration changed callback.
void DataSubscriber::RegisterConfigurationChangedCallback(ConfigurationChangedCallback configurationChangedCallback)
{
    m_configurationChangedCallback = configurationChangedCallback;
}

// Registers the connection terminated callback.
void DataSubscriber::RegisterConnectionTerminatedCallback(ConnectionTerminatedCallback connectionTerminatedCallback)
{
    m_connectionTerminatedCallback = connectionTerminatedCallback;
}

// Registers the auto-reconnect callback.
void DataSubscriber::RegisterAutoReconnectCallback(ConnectionTerminatedCallback autoReconnectCallback)
{
    m_autoReconnectCallback = autoReconnectCallback;
}

// Returns true if payload data is compressed (TSSC only).
bool DataSubscriber::IsPayloadDataCompressed() const
{
    return m_compressPayloadData;
}

// Set the value which determines whether payload data is compressed.
void DataSubscriber::SetPayloadDataCompressed(bool compressed)
{
    // This operational mode can only be changed before connect - dynamic updates not supported
    m_compressPayloadData = compressed;
}

// Returns true if metadata exchange is compressed (GZip only).
bool DataSubscriber::IsMetadataCompressed() const
{
    return m_compressMetadata;
}

// Set the value which determines whether metadata exchange is compressed.
void DataSubscriber::SetMetadataCompressed(bool compressed)
{
    m_compressMetadata = compressed;

    if (m_commandChannelSocket.is_open())
        SendOperationalModes();
}

// Returns true if signal index cache exchange is compressed (GZip only).
bool DataSubscriber::IsSignalIndexCacheCompressed() const
{
    return m_compressSignalIndexCache;
}

// Set the value which determines whether signal index cache exchange is compressed.
void DataSubscriber::SetSignalIndexCacheCompressed(bool compressed)
{
    m_compressSignalIndexCache = compressed;

    if (m_commandChannelSocket.is_open())
        SendOperationalModes();
}

// Gets user defined data reference
void* DataSubscriber::GetUserData() const
{
    return m_userData;
}

// Sets user defined data reference
void DataSubscriber::SetUserData(void* userData)
{
    m_userData = userData;
}

SubscriberConnector& DataSubscriber::GetSubscriberConnector()
{
    return m_connector;
}

// Returns the subscription info object used to define the most recent subscription.
const SubscriptionInfo& DataSubscriber::GetSubscriptionInfo() const
{
    return m_subscriptionInfo;
}

void DataSubscriber::SetSubscriptionInfo(const SubscriptionInfo& info)
{
    m_subscriptionInfo = info;
}

// Synchronously connects to publisher.
void DataSubscriber::Connect(string hostname, uint16_t port)
{
    DnsResolver resolver(m_commandChannelService);
    const DnsResolver::query query(hostname, to_string(port));
    const DnsResolver::iterator endpointIterator = resolver.resolve(query);
    DnsResolver::iterator hostEndpoint;
    ErrorCode error;

    m_totalCommandChannelBytesReceived = 0UL;
    m_totalDataChannelBytesReceived = 0UL;
    m_totalMeasurementsReceived = 0UL;

    if (m_connected)
        throw SubscriberException("Subscriber is already connected; disconnect first");

    hostEndpoint = connect(m_commandChannelSocket, endpointIterator, error);

    if (error)
        throw SystemError(error);

    if (!m_commandChannelSocket.is_open())
        throw SubscriberException("Failed to connect to host");

    m_hostAddress = hostEndpoint->endpoint().address();

    m_commandChannelService.restart();
    m_callbackThread = Thread(bind(&DataSubscriber::RunCallbackThread, this));
    m_commandChannelResponseThread = Thread(bind(&DataSubscriber::RunCommandChannelResponseThread, this));

    SendOperationalModes();
    m_connected = true;
}

void DataSubscriber::Disconnect(bool autoReconnect)
{
    ErrorCode error;

    // Notify running threads that
    // the subscriber is disconnecting
    m_disconnecting = true;
    m_connected = false;
    m_subscribed = false;

    // Release queues and close sockets so
    // that threads can shut down gracefully
    m_callbackQueue.Release();
    m_commandChannelSocket.close(error);
    m_dataChannelSocket.shutdown(UdpSocket::shutdown_receive, error);
    m_dataChannelSocket.close(error);

    // Join with all threads to guarantee their completion
    // before returning control to the caller
    m_callbackThread.join();
    m_commandChannelResponseThread.join();
    m_dataChannelResponseThread.join();

    // Empty queues and reset them so they can be used
    // again later if the user decides to reconnect
    m_callbackQueue.Clear();
    m_callbackQueue.Reset();

    // Notify consumers of disconnect
    if (m_connectionTerminatedCallback != nullptr)
        m_connectionTerminatedCallback(this);

    if (autoReconnect)
    {
        // Handling auto-connect callback separately from connection terminated callback
        // since they serve two different use cases and current implementation does not
        // support multiple callback registrations
        if (m_autoReconnectCallback != nullptr)
            m_autoReconnectCallback(this);
    }
    else
    {
        m_connector.Cancel();
    }

    // Disconnect completed
    m_disconnecting = false;
}

// Disconnects from the publisher.
void DataSubscriber::Disconnect()
{
    // User requests to disconnect should not auto-reconnect
    Disconnect(false);
}

void DataSubscriber::Subscribe(const SubscriptionInfo info)
{
    SetSubscriptionInfo(info);
    Subscribe();
}

// Subscribe to publisher in order to start receiving data.
void DataSubscriber::Subscribe()
{
    udp ipVersion = udp::v4();

    stringstream connectionStream;
    string connectionString;

    vector<uint8_t> buffer;
    uint8_t* connectionStringPtr;
    uint32_t connectionStringSize;
    uint32_t bigEndianConnectionStringSize;
    uint8_t* bigEndianConnectionStringSizePtr;

    uint32_t bufferSize;
    uint32_t i;

    // Make sure to unsubscribe before attempting another
    // subscription so we don't leave connections open
    if (m_subscribed)
        Unsubscribe();

    m_totalMeasurementsReceived = 0UL;

    connectionStream << "trackLatestMeasurements=" << m_subscriptionInfo.Throttled << ";";
    connectionStream << "includeTime=" << m_subscriptionInfo.IncludeTime << ";";
    connectionStream << "lagTime=" << m_subscriptionInfo.LagTime << ";";
    connectionStream << "leadTime=" << m_subscriptionInfo.LeadTime << ";";
    connectionStream << "useLocalClockAsRealTime=" << m_subscriptionInfo.UseLocalClockAsRealTime << ";";
    connectionStream << "processingInterval=" << m_subscriptionInfo.ProcessingInterval << ";";
    connectionStream << "useMillisecondResolution=" << m_subscriptionInfo.UseMillisecondResolution << ";";
    connectionStream << "assemblyInfo={source=TimeSeriesPlatformLibrary; version=" GSFTS_VERSION "; buildDate=" GSFTS_BUILD_DATE "};";

    if (!m_subscriptionInfo.FilterExpression.empty())
        connectionStream << "inputMeasurementKeys={" << m_subscriptionInfo.FilterExpression << "};";

    if (m_subscriptionInfo.UdpDataChannel)
    {
        if (m_hostAddress.is_v6())
            ipVersion = udp::v6();

        // Attempt to bind to local UDP port
        m_dataChannelSocket.open(ipVersion);
        m_dataChannelSocket.bind(udp::endpoint(ipVersion, m_subscriptionInfo.DataChannelLocalPort));
        m_dataChannelResponseThread = Thread(bind(&DataSubscriber::RunDataChannelResponseThread, this));

        if (!m_dataChannelSocket.is_open())
            throw SubscriberException("Failed to bind to local port");

        connectionStream << "dataChannel={localport=" << m_subscriptionInfo.DataChannelLocalPort << "};";
    }

    if (!m_subscriptionInfo.StartTime.empty())
        connectionStream << "startTimeConstraint=" << m_subscriptionInfo.StartTime << ";";

    if (!m_subscriptionInfo.StopTime.empty())
        connectionStream << "stopTimeConstraint=" << m_subscriptionInfo.StopTime << ";";

    if (!m_subscriptionInfo.ConstraintParameters.empty())
        connectionStream << "timeConstraintParameters=" << m_subscriptionInfo.ConstraintParameters << ";";

    if (!m_subscriptionInfo.ExtraConnectionStringParameters.empty())
        connectionStream << m_subscriptionInfo.ExtraConnectionStringParameters << ";";

    connectionString = connectionStream.str();
    connectionStringPtr = reinterpret_cast<uint8_t*>(&connectionString[0]);
    connectionStringSize = static_cast<uint32_t>(connectionString.size() * sizeof(char));
    bigEndianConnectionStringSize = m_endianConverter.ConvertBigEndian(connectionStringSize);
    bigEndianConnectionStringSizePtr = reinterpret_cast<uint8_t*>(&bigEndianConnectionStringSize);

    bufferSize = 5 + connectionStringSize;
    buffer.resize(bufferSize, 0);

    buffer[0] = DataPacketFlags::Compact | (m_subscriptionInfo.RemotelySynchronized ? DataPacketFlags::Synchronized : DataPacketFlags::NoFlags);

    buffer[1] = bigEndianConnectionStringSizePtr[0];
    buffer[2] = bigEndianConnectionStringSizePtr[1];
    buffer[3] = bigEndianConnectionStringSizePtr[2];
    buffer[4] = bigEndianConnectionStringSizePtr[3];

    for (i = 0; i < connectionStringSize; ++i)
        buffer[5 + i] = connectionStringPtr[i];

    SendServerCommand(ServerCommand::Subscribe, &buffer[0], 0, bufferSize);

    // Reset TSSC decompresser on successful (re)subscription
    m_tsscResetRequested = true;
}

// Unsubscribe from publisher to stop receiving data.
void DataSubscriber::Unsubscribe()
{
    ErrorCode error;

    m_disconnecting = true;
    m_dataChannelSocket.shutdown(UdpSocket::shutdown_receive, error);
    m_dataChannelSocket.close(error);
    m_dataChannelResponseThread.join();
    m_disconnecting = false;

    SendServerCommand(ServerCommand::Unsubscribe);
}

// Sends a command to the server.
void DataSubscriber::SendServerCommand(uint8_t commandCode)
{
    SendServerCommand(commandCode, nullptr, 0, 0);
}

// Sends a command along with the given message to the server.
void DataSubscriber::SendServerCommand(uint8_t commandCode, string message)
{
    uint32_t bufferSize;
    vector<uint8_t> buffer;

    uint8_t* messagePtr;
    uint32_t messageSize;
    uint32_t bigEndianMessageSize;
    uint8_t* bigEndianMessageSizePtr;

    messagePtr = reinterpret_cast<uint8_t*>(&message[0]);
    messageSize = static_cast<uint32_t>(message.size() * sizeof(char));
    bigEndianMessageSize = m_endianConverter.ConvertBigEndian(messageSize);
    bigEndianMessageSizePtr = reinterpret_cast<uint8_t*>(&bigEndianMessageSize);

    bufferSize = 4 + messageSize;
    buffer.resize(bufferSize, 0);

    buffer[0] = bigEndianMessageSizePtr[0];
    buffer[1] = bigEndianMessageSizePtr[1];
    buffer[2] = bigEndianMessageSizePtr[2];
    buffer[3] = bigEndianMessageSizePtr[3];

    for (uint32_t i = 0; i < messageSize; ++i)
        buffer[4 + i] = messagePtr[i];

    SendServerCommand(commandCode, &buffer[0], 0, bufferSize);
}

// Sends a command along with the given data to the server.
void DataSubscriber::SendServerCommand(uint8_t commandCode, const uint8_t* data, uint32_t offset, uint32_t length)
{
    const uint32_t packetSize = 1 + static_cast<uint32_t>(length);
    uint32_t littleEndianPacketSize = m_endianConverter.ConvertLittleEndian(packetSize);
    uint8_t* littleEndianPacketSizePtr = reinterpret_cast<uint8_t*>(&littleEndianPacketSize);
    const uint32_t commandBufferSize = packetSize + 8U;

    if (commandBufferSize > static_cast<uint32_t>(m_writeBuffer.size()))
        m_writeBuffer.resize(commandBufferSize);

    // Insert payload marker
    m_writeBuffer[0] = 0xAA;
    m_writeBuffer[1] = 0xBB;
    m_writeBuffer[2] = 0xCC;
    m_writeBuffer[3] = 0xDD;

    // Insert packet size
    m_writeBuffer[4] = littleEndianPacketSizePtr[0];
    m_writeBuffer[5] = littleEndianPacketSizePtr[1];
    m_writeBuffer[6] = littleEndianPacketSizePtr[2];
    m_writeBuffer[7] = littleEndianPacketSizePtr[3];

    // Insert command code
    m_writeBuffer[8] = commandCode;

    if (data != nullptr)
    {
        for (uint32_t i = 0; i < length; ++i)
            m_writeBuffer[9 + i] = data[offset + i];
    }

    async_write(m_commandChannelSocket, buffer(m_writeBuffer, commandBufferSize), &WriteHandler);
}

// Convenience method to send the currently defined
// and/or supported operational modes to the server.
void DataSubscriber::SendOperationalModes()
{
    uint32_t operationalModes = CompressionModes::GZip;
    uint32_t bigEndianOperationalModes;

    operationalModes |= OperationalEncoding::UTF8;
    operationalModes |= OperationalModes::UseCommonSerializationFormat;

    // TSSC compression only works with stateful connections
    if (m_compressPayloadData && !m_subscriptionInfo.UdpDataChannel)
        operationalModes |= OperationalModes::CompressPayloadData | CompressionModes::TSSC;

    if (m_compressMetadata)
        operationalModes |= OperationalModes::CompressMetadata;

    if (m_compressSignalIndexCache)
        operationalModes |= OperationalModes::CompressSignalIndexCache;

    bigEndianOperationalModes = m_endianConverter.ConvertBigEndian(operationalModes);
    SendServerCommand(ServerCommand::DefineOperationalModes, reinterpret_cast<uint8_t*>(&bigEndianOperationalModes), 0, 4);
}

// Gets the total number of bytes received via the command channel since last connection.
uint64_t DataSubscriber::GetTotalCommandChannelBytesReceived() const
{
    return m_totalCommandChannelBytesReceived;
}

// Gets the total number of bytes received via the data channel since last connection.
uint64_t DataSubscriber::GetTotalDataChannelBytesReceived() const
{
    if (m_subscriptionInfo.UdpDataChannel)
        return m_totalDataChannelBytesReceived;

    return m_totalCommandChannelBytesReceived;
}

// Gets the total number of measurements received since last subscription.
uint64_t DataSubscriber::GetTotalMeasurementsReceived() const
{
    return m_totalMeasurementsReceived;
}

// Indicates whether the subscriber is connected.
bool DataSubscriber::IsConnected() const
{
    return m_connected;
}

// Indicates whether the subscriber is subscribed.
bool DataSubscriber::IsSubscribed() const
{
    return m_subscribed;
}

// --- Convenience Methods ---

// This method does nothing. It is used as the callback for asynchronous write operations.
void WriteHandler(const ErrorCode& error, uint32_t bytesTransferred)
{
}