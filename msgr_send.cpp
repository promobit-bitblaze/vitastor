// Copyright (c) Vitaliy Filippov, 2019+
// License: VNPL-1.0 or GNU GPL-2.0+ (see README.md for details)

#include "messenger.h"

void osd_messenger_t::outbox_push(osd_op_t *cur_op)
{
    assert(cur_op->peer_fd);
    osd_client_t *cl = clients.at(cur_op->peer_fd);
    if (cur_op->op_type == OSD_OP_OUT)
    {
        clock_gettime(CLOCK_REALTIME, &cur_op->tv_begin);
    }
    else
    {
        // Check that operation actually belongs to this client
        // FIXME: Review if this is still needed
        bool found = false;
        for (auto it = cl->received_ops.begin(); it != cl->received_ops.end(); it++)
        {
            if (*it == cur_op)
            {
                found = true;
                cl->received_ops.erase(it, it+1);
                break;
            }
        }
        if (!found)
        {
            delete cur_op;
            return;
        }
    }
    auto & to_send_list = cl->write_msg.msg_iovlen ? cl->next_send_list : cl->send_list;
    auto & to_outbox = cl->write_msg.msg_iovlen ? cl->next_outbox : cl->outbox;
    if (cur_op->op_type == OSD_OP_IN)
    {
        measure_exec(cur_op);
        to_send_list.push_back((iovec){ .iov_base = cur_op->reply.buf, .iov_len = OSD_PACKET_SIZE });
    }
    else
    {
        to_send_list.push_back((iovec){ .iov_base = cur_op->req.buf, .iov_len = OSD_PACKET_SIZE });
        cl->sent_ops[cur_op->req.hdr.id] = cur_op;
    }
    // Pre-defined send_lists
    if ((cur_op->op_type == OSD_OP_IN
        ? (cur_op->req.hdr.opcode == OSD_OP_READ ||
        cur_op->req.hdr.opcode == OSD_OP_SEC_READ ||
        cur_op->req.hdr.opcode == OSD_OP_SEC_LIST ||
        cur_op->req.hdr.opcode == OSD_OP_SHOW_CONFIG)
        : (cur_op->req.hdr.opcode == OSD_OP_WRITE ||
        cur_op->req.hdr.opcode == OSD_OP_SEC_WRITE ||
        cur_op->req.hdr.opcode == OSD_OP_SEC_WRITE_STABLE ||
        cur_op->req.hdr.opcode == OSD_OP_SEC_STABILIZE ||
        cur_op->req.hdr.opcode == OSD_OP_SEC_ROLLBACK)) && cur_op->iov.count > 0)
    {
        to_outbox.push_back(NULL);
        for (int i = 0; i < cur_op->iov.count; i++)
        {
            assert(cur_op->iov.buf[i].iov_base);
            to_send_list.push_back(cur_op->iov.buf[i]);
            to_outbox.push_back(i == cur_op->iov.count-1 ? cur_op : NULL);
        }
    }
    else
    {
        to_outbox.push_back(cur_op);
    }
    if (!ringloop)
    {
        // FIXME: It's worse because it doesn't allow batching
        while (cl->outbox.size())
        {
            try_send(cl);
        }
    }
    else if (cl->write_msg.msg_iovlen > 0 || !try_send(cl))
    {
        if (cl->write_state == 0)
        {
            cl->write_state = CL_WRITE_READY;
            write_ready_clients.push_back(cur_op->peer_fd);
        }
        ringloop->wakeup();
    }
}

void osd_messenger_t::measure_exec(osd_op_t *cur_op)
{
    // Measure execution latency
    timespec tv_end;
    clock_gettime(CLOCK_REALTIME, &tv_end);
    stats.op_stat_count[cur_op->req.hdr.opcode]++;
    if (!stats.op_stat_count[cur_op->req.hdr.opcode])
    {
        stats.op_stat_count[cur_op->req.hdr.opcode]++;
        stats.op_stat_sum[cur_op->req.hdr.opcode] = 0;
        stats.op_stat_bytes[cur_op->req.hdr.opcode] = 0;
    }
    stats.op_stat_sum[cur_op->req.hdr.opcode] += (
        (tv_end.tv_sec - cur_op->tv_begin.tv_sec)*1000000 +
        (tv_end.tv_nsec - cur_op->tv_begin.tv_nsec)/1000
    );
    if (cur_op->req.hdr.opcode == OSD_OP_READ ||
        cur_op->req.hdr.opcode == OSD_OP_WRITE)
    {
        stats.op_stat_bytes[cur_op->req.hdr.opcode] += cur_op->req.rw.len;
    }
    else if (cur_op->req.hdr.opcode == OSD_OP_SEC_READ ||
        cur_op->req.hdr.opcode == OSD_OP_SEC_WRITE ||
        cur_op->req.hdr.opcode == OSD_OP_SEC_WRITE_STABLE)
    {
        stats.op_stat_bytes[cur_op->req.hdr.opcode] += cur_op->req.sec_rw.len;
    }
}

bool osd_messenger_t::try_send(osd_client_t *cl)
{
    int peer_fd = cl->peer_fd;
    if (!cl->send_list.size() || cl->write_msg.msg_iovlen > 0)
    {
        return true;
    }
    if (ringloop && !use_sync_send_recv)
    {
        io_uring_sqe* sqe = ringloop->get_sqe();
        if (!sqe)
        {
            return false;
        }
        cl->write_msg.msg_iov = cl->send_list.data();
        cl->write_msg.msg_iovlen = cl->send_list.size();
        cl->refs++;
        ring_data_t* data = ((ring_data_t*)sqe->user_data);
        data->callback = [this, cl](ring_data_t *data) { handle_send(data->res, cl); };
        my_uring_prep_sendmsg(sqe, peer_fd, &cl->write_msg, 0);
    }
    else
    {
        cl->write_msg.msg_iov = cl->send_list.data();
        cl->write_msg.msg_iovlen = cl->send_list.size();
        cl->refs++;
        int result = sendmsg(peer_fd, &cl->write_msg, MSG_NOSIGNAL);
        if (result < 0)
        {
            result = -errno;
        }
        handle_send(result, cl);
    }
    return true;
}

void osd_messenger_t::send_replies()
{
    for (int i = 0; i < write_ready_clients.size(); i++)
    {
        int peer_fd = write_ready_clients[i];
        auto cl_it = clients.find(peer_fd);
        if (cl_it != clients.end() && !try_send(cl_it->second))
        {
            write_ready_clients.erase(write_ready_clients.begin(), write_ready_clients.begin() + i);
            return;
        }
    }
    write_ready_clients.clear();
}

void osd_messenger_t::handle_send(int result, osd_client_t *cl)
{
    cl->write_msg.msg_iovlen = 0;
    cl->refs--;
    if (cl->peer_state == PEER_STOPPED)
    {
        if (!cl->refs)
        {
            delete cl;
        }
        return;
    }
    if (result < 0 && result != -EAGAIN)
    {
        // this is a client socket, so don't panic. just disconnect it
        printf("Client %d socket write error: %d (%s). Disconnecting client\n", cl->peer_fd, -result, strerror(-result));
        stop_client(cl->peer_fd);
        return;
    }
    if (result >= 0)
    {
        int done = 0;
        while (result > 0 && done < cl->send_list.size())
        {
            iovec & iov = cl->send_list[done];
            if (iov.iov_len <= result)
            {
                if (cl->outbox[done])
                {
                    // Operation fully sent
                    if (cl->outbox[done]->op_type == OSD_OP_IN)
                    {
                        delete cl->outbox[done];
                    }
                }
                result -= iov.iov_len;
                done++;
            }
            else
            {
                iov.iov_len -= result;
                iov.iov_base += result;
                break;
            }
        }
        if (done > 0)
        {
            cl->send_list.erase(cl->send_list.begin(), cl->send_list.begin()+done);
            cl->outbox.erase(cl->outbox.begin(), cl->outbox.begin()+done);
        }
        if (cl->next_send_list.size())
        {
            cl->send_list.insert(cl->send_list.end(), cl->next_send_list.begin(), cl->next_send_list.end());
            cl->outbox.insert(cl->outbox.end(), cl->next_outbox.begin(), cl->next_outbox.end());
            cl->next_send_list.clear();
            cl->next_outbox.clear();
        }
        cl->write_state = cl->outbox.size() > 0 ? CL_WRITE_READY : 0;
    }
    if (cl->write_state != 0)
    {
        write_ready_clients.push_back(cl->peer_fd);
    }
}
