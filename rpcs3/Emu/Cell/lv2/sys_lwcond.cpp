#include "stdafx.h"
#include "sys_lwcond.h"

#include "Emu/IdManager.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"
#include "sys_lwmutex.h"

LOG_CHANNEL(sys_lwcond);

lv2_lwcond::lv2_lwcond(utils::serial& ar)
	: name(ar.operator be_t<u64>())
	, lwid(ar)
	, protocol(ar)
	, control(ar.operator decltype(control)())
{
}

void lv2_lwcond::save(utils::serial& ar)
{
	USING_SERIALIZATION_VERSION(lv2_sync);
	ar(name, lwid, protocol, control);
}

error_code _sys_lwcond_create(ppu_thread& ppu, vm::ptr<u32> lwcond_id, u32 lwmutex_id, vm::ptr<sys_lwcond_t> control, u64 name)
{
	ppu.state += cpu_flag::wait;

	sys_lwcond.warning(u8"_sys_lwcond_create(lwcond_id=*0x%x, lwmutex_id=0x%x, control=*0x%x, name=0x%llx (“%s”))", lwcond_id, lwmutex_id, control, name, lv2_obj::name64(std::bit_cast<be_t<u64>>(name)));

	u32 protocol;

	// Extract protocol from lwmutex
	if (!idm::check<lv2_obj, lv2_lwmutex>(lwmutex_id, [&protocol](lv2_lwmutex& mutex)
	{
		protocol = mutex.protocol;
	}))
	{
		return CELL_ESRCH;
	}

	if (protocol == SYS_SYNC_RETRY)
	{
		// Lwcond can't have SYS_SYNC_RETRY protocol
		protocol = SYS_SYNC_PRIORITY;
	}

	if (const u32 id = idm::make<lv2_obj, lv2_lwcond>(name, lwmutex_id, protocol, control))
	{
		*lwcond_id = id;
		return CELL_OK;
	}

	return CELL_EAGAIN;
}

error_code _sys_lwcond_destroy(ppu_thread& ppu, u32 lwcond_id)
{
	ppu.state += cpu_flag::wait;

	sys_lwcond.warning("_sys_lwcond_destroy(lwcond_id=0x%x)", lwcond_id);

	const auto cond = idm::withdraw<lv2_obj, lv2_lwcond>(lwcond_id, [&](lv2_lwcond& cond) -> CellError
	{
		if (cond.waiters)
		{
			return CELL_EBUSY;
		}

		return {};
	});

	if (!cond)
	{
		return CELL_ESRCH;
	}

	if (cond.ret)
	{
		return cond.ret;
	}

	return CELL_OK;
}

error_code _sys_lwcond_signal(ppu_thread& ppu, u32 lwcond_id, u32 lwmutex_id, u64 ppu_thread_id, u32 mode)
{
	ppu.state += cpu_flag::wait;

	sys_lwcond.trace("_sys_lwcond_signal(lwcond_id=0x%x, lwmutex_id=0x%x, ppu_thread_id=0x%llx, mode=%d)", lwcond_id, lwmutex_id, ppu_thread_id, mode);

	// Mode 1: lwmutex was initially owned by the calling thread
	// Mode 2: lwmutex was not owned by the calling thread and waiter hasn't been increased
	// Mode 3: lwmutex was forcefully owned by the calling thread

	if (mode < 1 || mode > 3)
	{
		fmt::throw_exception("Unknown mode (%d)", mode);
	}

	const auto cond = idm::check<lv2_obj, lv2_lwcond>(lwcond_id, [&](lv2_lwcond& cond) -> int
	{
		ppu_thread* cpu = nullptr;

		if (ppu_thread_id != u32{umax})
		{
			cpu = idm::check_unlocked<named_thread<ppu_thread>>(static_cast<u32>(ppu_thread_id));

			if (!cpu)
			{
				return -1;
			}
		}

		lv2_lwmutex* mutex = nullptr;

		if (mode != 2)
		{
			mutex = idm::check_unlocked<lv2_obj, lv2_lwmutex>(lwmutex_id);

			if (!mutex)
			{
				return -1;
			}
		}

		if (cond.waiters)
		{
			std::lock_guard lock(cond.mutex);

			if (cpu)
			{
				if (static_cast<ppu_thread*>(cpu)->state & cpu_flag::again)
				{
					ppu.state += cpu_flag::again;
					return 0;
				}
			}

			auto result = cpu ? cond.unqueue(cond.sq, cpu) :
				cond.schedule<ppu_thread>(cond.sq, cond.protocol);

			if (result)
			{
				if (static_cast<ppu_thread*>(result)->state & cpu_flag::again)
				{
					ppu.state += cpu_flag::again;
					return 0;
				}

				cond.waiters--;

				if (mode == 2)
				{
					static_cast<ppu_thread*>(result)->gpr[3] = CELL_EBUSY;
				}

				if (mode != 2)
				{
					ensure(!mutex->signaled);
					std::lock_guard lock(mutex->mutex);

					if (mode == 3 && !mutex->sq.empty()) [[unlikely]]
					{
						// Respect ordering of the sleep queue
						mutex->sq.emplace_back(result);
						result = mutex->schedule<ppu_thread>(mutex->sq, mutex->protocol);

						if (static_cast<ppu_thread*>(result)->state & cpu_flag::again)
						{
							ppu.state += cpu_flag::again;
							return 0;
						}
					}
					else if (mode == 1)
					{
						mutex->add_waiter(result);
						result = nullptr;
					}
				}

				if (result)
				{
					cond.awake(result);
				}

				return 1;
			}
		}

		return 0;
	});

	if (!cond || cond.ret == -1)
	{
		return CELL_ESRCH;
	}

	if (!cond.ret)
	{
		if (ppu_thread_id == u32{umax})
		{
			if (mode == 3)
			{
				return not_an_error(CELL_ENOENT);
			}
			else if (mode == 2)
			{
				return CELL_OK;
			}
		}

		return not_an_error(CELL_EPERM);
	}

	return CELL_OK;
}

error_code _sys_lwcond_signal_all(ppu_thread& ppu, u32 lwcond_id, u32 lwmutex_id, u32 mode)
{
	ppu.state += cpu_flag::wait;

	sys_lwcond.trace("_sys_lwcond_signal_all(lwcond_id=0x%x, lwmutex_id=0x%x, mode=%d)", lwcond_id, lwmutex_id, mode);

	// Mode 1: lwmutex was initially owned by the calling thread
	// Mode 2: lwmutex was not owned by the calling thread and waiter hasn't been increased

	if (mode < 1 || mode > 2)
	{
		fmt::throw_exception("Unknown mode (%d)", mode);
	}

	bool need_awake = false;

	const auto cond = idm::check<lv2_obj, lv2_lwcond>(lwcond_id, [&](lv2_lwcond& cond) -> s32
	{
		lv2_lwmutex* mutex{};

		if (mode != 2)
		{
			mutex = idm::check_unlocked<lv2_obj, lv2_lwmutex>(lwmutex_id);

			if (!mutex)
			{
				return -1;
			}
		}

		if (cond.waiters)
		{
			std::lock_guard lock(cond.mutex);

			u32 result = 0;

			for (auto cpu : cond.sq)
			{
				if (static_cast<ppu_thread*>(cpu)->state & cpu_flag::again)
				{
					ppu.state += cpu_flag::again;
					return 0;
				}
			}

			cond.waiters = 0;

			while (const auto cpu = cond.schedule<ppu_thread>(cond.sq, cond.protocol))
			{
				if (mode == 2)
				{
					static_cast<ppu_thread*>(cpu)->gpr[3] = CELL_EBUSY;
				}

				if (mode == 1)
				{
					ensure(!mutex->signaled);
					std::lock_guard lock(mutex->mutex);
					mutex->add_waiter(cpu);
				}
				else
				{
					lv2_obj::append(cpu);
					need_awake = true;
				}

				result++;
			}

			if (need_awake)
			{
				lv2_obj::awake_all();
			}

			return result;
		}

		return 0;
	});

	if (!cond || cond.ret == -1)
	{
		return CELL_ESRCH;
	}

	if (mode == 1)
	{
		// Mode 1: return the amount of threads (TODO)
		return not_an_error(cond.ret);
	}

	return CELL_OK;
}

error_code _sys_lwcond_queue_wait(ppu_thread& ppu, u32 lwcond_id, u32 lwmutex_id, u64 timeout)
{
	ppu.state += cpu_flag::wait;

	sys_lwcond.trace("_sys_lwcond_queue_wait(lwcond_id=0x%x, lwmutex_id=0x%x, timeout=0x%llx)", lwcond_id, lwmutex_id, timeout);

	ppu.gpr[3] = CELL_OK;

	std::shared_ptr<lv2_lwmutex> mutex;

	auto& sstate = *ppu.optional_savestate_state;

	const auto cond = idm::get<lv2_obj, lv2_lwcond>(lwcond_id, [&](lv2_lwcond& cond)
	{
		mutex = idm::get_unlocked<lv2_obj, lv2_lwmutex>(lwmutex_id);

		if (!mutex)
		{
			return;
		}

		// Increment lwmutex's lwcond's waiters count
		mutex->lwcond_waiters++;

		std::lock_guard lock(cond.mutex);

		const bool mutex_sleep = sstate.try_read<bool>().second;
		sstate.clear();

		if (mutex_sleep)
		{
			// Special: loading state from the point of waiting on lwmutex sleep queue
			std::lock_guard lock2(mutex->mutex);
			mutex->sq.emplace_back(&ppu);
		}
		else
		{
			// Add a waiter
			cond.waiters++;
			cond.sq.emplace_back(&ppu);
		}

		if (!ppu.loaded_from_savestate)
		{
			std::lock_guard lock2(mutex->mutex);

			// Process lwmutex sleep queue
			if (const auto cpu = mutex->schedule<ppu_thread>(mutex->sq, mutex->protocol))
			{
				if (static_cast<ppu_thread*>(cpu)->state & cpu_flag::again)
				{
					ppu.state += cpu_flag::again;
					return;
				}

				cond.append(cpu);
			}
			else
			{
				mutex->signaled |= 1;
			}
		}

		// Sleep current thread and schedule lwmutex waiter
		cond.sleep(ppu, timeout);
	});

	if (!cond || !mutex)
	{
		return CELL_ESRCH;
	}

	if (ppu.state & cpu_flag::again)
	{
		return CELL_OK;
	}

	while (auto state = ppu.state.fetch_sub(cpu_flag::signal))
	{
		if (state & cpu_flag::signal)
		{
			break;
		}

		if (is_stopped(state))
		{
			reader_lock lock(cond->mutex);
			reader_lock lock2(mutex->mutex);

			const bool cond_sleep = std::find(cond->sq.begin(), cond->sq.end(), &ppu) != cond->sq.end();
			const bool mutex_sleep = std::find(mutex->sq.begin(), mutex->sq.end(), &ppu) != mutex->sq.end();

			if (!cond_sleep && !mutex_sleep)
			{
				break;
			}

			sstate(mutex_sleep);
			ppu.state += cpu_flag::again;
			break;
		}

		if (timeout)
		{
			if (lv2_obj::wait_timeout(timeout, &ppu))
			{
				// Wait for rescheduling
				if (ppu.check_state())
				{
					continue;
				}

				std::lock_guard lock(cond->mutex);

				if (cond->unqueue(cond->sq, &ppu))
				{
					cond->waiters--;
					ppu.gpr[3] = CELL_ETIMEDOUT;
					break;
				}

				reader_lock lock2(mutex->mutex);

				if (std::find(mutex->sq.cbegin(), mutex->sq.cend(), &ppu) == mutex->sq.cend())
				{
					break;
				}

				mutex->sleep(ppu);
				timeout = 0;
				continue;
			}
		}
		else
		{
			thread_ctrl::wait_on(ppu.state, state);
		}
	}

	if (--mutex->lwcond_waiters == smin)
	{
		// Notify the thread destroying lwmutex on last waiter
		mutex->lwcond_waiters.notify_all();
	}

	// Return cause
	return not_an_error(ppu.gpr[3]);
}
