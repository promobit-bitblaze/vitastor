#!/bin/bash -ex

if [ ! "$BASH_VERSION" ] ; then
    echo "Use bash to run this script ($0)" 1>&2
    exit 1
fi

format_error()
{
    echo $(echo -n -e "\033[1;31m")"$1"$(echo -n -e "\033[m")
    $ETCDCTL get --prefix /vitastor > ./testdata/etcd-dump.txt
    exit 1
}
format_green()
{
    echo $(echo -n -e "\033[1;32m")"$1"$(echo -n -e "\033[m")
}

cd `dirname $0`/..

trap 'kill -9 $(jobs -p)' EXIT

ETCD=${ETCD:-etcd}
ETCD_IP=${ETCD_IP:-127.0.0.1}
ETCD_PORT=${ETCD_PORT:-12379}

if [ "$KEEP_DATA" = "" ]; then
    rm -rf ./testdata
    mkdir -p ./testdata
fi

$ETCD -name etcd_test --data-dir ./testdata/etcd \
    --advertise-client-urls http://$ETCD_IP:$ETCD_PORT --listen-client-urls http://$ETCD_IP:$ETCD_PORT \
    --initial-advertise-peer-urls http://$ETCD_IP:$((ETCD_PORT+1)) --listen-peer-urls http://$ETCD_IP:$((ETCD_PORT+1)) \
    --max-txn-ops=100000 --auto-compaction-retention=10 --auto-compaction-mode=revision &>./testdata/etcd.log &
ETCD_PID=$!
ETCD_URL=$ETCD_IP:$ETCD_PORT/v3
ETCDCTL="${ETCD}ctl --endpoints=http://$ETCD_URL"

echo leak:fio >> testdata/lsan-suppress.txt
echo leak:tcmalloc >> testdata/lsan-suppress.txt
echo leak:ceph >> testdata/lsan-suppress.txt
echo leak:librbd >> testdata/lsan-suppress.txt
echo leak:_M_mutate >> testdata/lsan-suppress.txt
echo leak:_M_assign >> testdata/lsan-suppress.txt
export LSAN_OPTIONS=report_objects=true:suppressions=`pwd`/testdata/lsan-suppress.txt
export ASAN_OPTIONS=verify_asan_link_order=false
