#!/bin/bash

install_dependencies() 
{
    local runner="$1"

    if [ "$runner" == "self-hosted-ubuntu18" -o "$runner" == "self-hosted-ubuntu20" -o "$runner" == "self-hosted-ubuntu22" ]; then
        sudo apt-get update
        sudo apt-get install -y build-essential
    elif [ "$runner" == "self-hosted-centos7" -o "$runner" == "self-hosted-centos8" -o "$runner" == "self-hosted-redhat7" -o "$runner" == "self-hosted-redhat8" -o "$runner" == "self-hosted-redhat9" ]; then
        sudo yum groupinstall -y "Development Tools"
    fi
        
    install_error=$?
    if [ $install_error -ne 0 ]; then
        echo "[ERROR] Installing dependencies for runner machine failed!"
        exit 1
    fi
}

untar_unix_test_suite()
{
   local directory="/lib"

    if [ ! -d "$directory" ]; then
        sudo mkdir -p "$directory"
        if [ $? -ne 0 ]; then
            echo "[ERROR] Unable to create directory $directory. Exiting."
            exit 1
        fi
        echo "Directory '$directory' created."
    else
        echo "Directory '$directory' already exists."
    fi

    echo "Untarring UnixTestSuite.tar to /lib..."
    tar -xf "$GITHUB_WORKSPACE/UnixTestSuite.tar" -C /lib

    if [ $? -ne 0 ]; then
        echo "[ERROR] Unable to untar UnixTestSuite.tar."
        exit 1
    fi

    echo "UnixTestSuite.tar untarred successfully to /lib."
}

runs_on="$1"  # The value passed from the workflow.

install_dependencies "$runs_on"
untar_unix_test_suite
