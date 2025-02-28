/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Api.h>
#include <aws/crt/io/HostResolver.h>

#include <aws/greengrass/GreengrassCoreIpcClient.h>

#include "../../utils/CommandLineUtils.h"

using namespace Aws::Crt;
using namespace Aws::Greengrass;

/* Used to check that the publish has been received so that the demo can exit successfully. */
static std::atomic_bool s_publishReceived(false);

int main(int argc, char *argv[])
{
    /************************ Setup the Lib ****************************/
    /*
     * Do the global initialization for the API.
     */
    ApiHandle apiHandle;

    /*********************** Parse Arguments ***************************/
    Utils::CommandLineUtils cmdUtils = Utils::CommandLineUtils();
    cmdUtils.RegisterProgramName("greengrass-ipc");
    cmdUtils.AddCommonTopicMessageCommands();
    const char **const_argv = (const char **)argv;
    cmdUtils.SendArguments(const_argv, const_argv + argc);

    String topic = cmdUtils.GetCommandOrDefault("topic", "test/topic");
    String message = cmdUtils.GetCommandOrDefault("message", "Hello World");

    /**
     * Create the default ClientBootstrap, which will create the default
     * EventLoopGroup (to process IO events) and HostResolver.
     */
    if (apiHandle.GetOrCreateStaticDefaultClientBootstrap()->LastError() != AWS_ERROR_SUCCESS)
    {
        fprintf(
            stderr,
            "ClientBootstrap failed with error %s\n",
            ErrorDebugString(apiHandle.GetOrCreateStaticDefaultClientBootstrap()->LastError()));
        exit(-1);
    }

    /*
     * Inheriting from ConnectionLifecycleHandler allows us to define callbacks that are
     * called upon when connection lifecycle events occur.
     */
    class SampleLifecycleHandler : public ConnectionLifecycleHandler
    {
      public:
        SampleLifecycleHandler() {}

        void OnConnectCallback() override { fprintf(stdout, "Connected to Greengrass Core\n"); }

        void OnDisconnectCallback(RpcError status) override
        {
            if (!status)
            {
                fprintf(stdout, "Disconnected from Greengrass Core with error: %s\n", status.StatusToString().c_str());
                exit(-1);
            }
        }

        bool OnErrorCallback(RpcError status) override
        {
            fprintf(
                stdout,
                "Processing messages from the Greengrass Core resulted in error: %s\n",
                status.StatusToString().c_str());
            return true;
        }
    };
    /*
     * Note: The lifecycle handler should be declared before the client
     * so that it is destroyed AFTER the client is destroyed.
     */
    SampleLifecycleHandler lifecycleHandler;
    GreengrassCoreIpcClient client(*apiHandle.GetOrCreateStaticDefaultClientBootstrap());
    auto connectionStatus = client.Connect(lifecycleHandler).get();

    if (!connectionStatus)
    {
        fprintf(stderr, "Failed to establish connection with error %s\n", connectionStatus.StatusToString().c_str());
        exit(-1);
    }

    /*
     * Upon receiving a message on the topic, print it and set an atomic bool so that the demo can complete.
     */
    class SubscribeStreamHandler : public SubscribeToIoTCoreStreamHandler
    {
        void OnStreamEvent(IoTCoreMessage *response) override
        {
            auto message = response->GetMessage();

            if (message.has_value() && message.value().GetPayload().has_value())
            {
                auto payloadBytes = message.value().GetPayload().value();
                std::string payloadString(payloadBytes.begin(), payloadBytes.end());
                fprintf(stdout, "Received payload: %s\n", payloadString.c_str());
            }

            s_publishReceived.store(true);
        };
    };

    SubscribeStreamHandler streamHandler;
    auto subscribeOperation = client.NewSubscribeToIoTCore(streamHandler);
    SubscribeToIoTCoreRequest subscribeRequest;
    subscribeRequest.SetQos(QOS_AT_LEAST_ONCE);
    subscribeRequest.SetTopicName(topic);

    fprintf(stdout, "Attempting to subscribe to %s topic\n", topic.c_str());
    auto requestStatus = subscribeOperation.Activate(subscribeRequest).get();
    if (!requestStatus)
    {
        fprintf(stderr, "Failed to send subscription request to %s topic\n", topic.c_str());
        exit(-1);
    }

    auto subscribeResultFuture = subscribeOperation.GetResult();
    /*
    // To avoid throwing exceptions, wait on the result for a specified timeout:
    if (subscribeResultFuture.wait_for(std::chrono::seconds(10)) == std::future_status::timeout)
    {
        fprintf(stderr, "Timed out while waiting for response from Greengrass Core\n");
        exit(-1);
    }
    */

    auto subscribeResult = subscribeResultFuture.get();
    if (subscribeResult)
    {
        fprintf(stdout, "Successfully subscribed to %s topic\n", topic.c_str());
    }
    else
    {
        auto errorType = subscribeResult.GetResultType();
        if (errorType == OPERATION_ERROR)
        {
            OperationError *error = subscribeResult.GetOperationError();
            /*
             * This pointer can be casted to any error type like so:
             * if(error->GetModelName() == UnauthorizedError::MODEL_NAME)
             *    UnauthorizedError *unauthorizedError = static_cast<UnauthorizedError*>(error);
             */
            if (error->GetMessage().has_value())
                fprintf(stderr, "Greengrass Core responded with an error: %s\n", error->GetMessage().value().c_str());
        }
        else
        {
            fprintf(
                stderr,
                "Attempting to receive the response from the server failed with error code %s\n",
                subscribeResult.GetRpcError().StatusToString().c_str());
        }
    }

    /* Publish to the same topic that is currently subscribed to. */
    auto publishOperation = client.NewPublishToIoTCore();
    PublishToIoTCoreRequest publishRequest;
    publishRequest.SetTopicName(topic);
    Vector<uint8_t> payload(message.begin(), message.end());
    publishRequest.SetPayload(payload);
    publishRequest.SetQos(QOS_AT_LEAST_ONCE);

    fprintf(stdout, "Attempting to publish to %s topic\n", topic.c_str());
    requestStatus = publishOperation.Activate(publishRequest).get();
    if (!requestStatus)
    {
        fprintf(
            stderr,
            "Failed to publish to %s topic with error %s\n",
            topic.c_str(),
            requestStatus.StatusToString().c_str());
        exit(-1);
    }

    auto publishResultFuture = publishOperation.GetResult();
    /*
    // To avoid throwing exceptions, wait on the result for a specified timeout:
    if (publishResultFuture.wait_for(std::chrono::seconds(10)) == std::future_status::timeout)
    {
        fprintf(stderr, "Timed out while waiting for response from Greengrass Core\n");
        exit(-1);
    }
    */

    auto publishResult = publishResultFuture.get();
    if (publishResult)
    {
        fprintf(stdout, "Successfully published to %s topic\n", topic.c_str());
        auto *response = publishResult.GetOperationResponse();
        (void)response;
    }
    else
    {
        auto errorType = publishResult.GetResultType();
        if (errorType == OPERATION_ERROR)
        {
            OperationError *error = publishResult.GetOperationError();
            /*
             * This pointer can be casted to any error type like so:
             * if(error->GetModelName() == UnauthorizedError::MODEL_NAME)
             *    UnauthorizedError *unauthorizedError = static_cast<UnauthorizedError*>(error);
             */
            if (error->GetMessage().has_value())
                fprintf(stderr, "Greengrass Core responded with an error: %s\n", error->GetMessage().value().c_str());
        }
        else
        {
            fprintf(
                stderr,
                "Attempting to receive the response from the server failed with error code %s\n",
                publishResult.GetRpcError().StatusToString().c_str());
        }
    }

    /* Wait for the publish to be received since this sample subscribes to the same topic it publishes to. */
    while (!s_publishReceived.load())
    {
        continue;
    }

    return 0;
}
