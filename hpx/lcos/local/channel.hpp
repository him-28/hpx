//  Copyright (c) 2016 Hartmut Kaiser
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_LCOS_LOCAL_CHANNEL_JUL_23_2016_0707PM)
#define HPX_LCOS_LOCAL_CHANNEL_JUL_23_2016_0707PM

#include <hpx/config.hpp>
#include <hpx/exception.hpp>
#include <hpx/lcos/future.hpp>
#include <hpx/lcos/local/receive_buffer.hpp>
#include <hpx/lcos/local/spinlock.hpp>
#include <hpx/util/assert.hpp>
#include <hpx/util/atomic_count.hpp>
#include <hpx/util/iterator_facade.hpp>
#include <hpx/util/unused.hpp>

#include <boost/intrusive_ptr.hpp>

#include <cstdlib>
#include <iterator>
#include <mutex>
#include <utility>

namespace hpx { namespace lcos { namespace local
{
    ///////////////////////////////////////////////////////////////////////////
    namespace detail
    {
        ///////////////////////////////////////////////////////////////////////
        template <typename T>
        struct channel_base
        {
            channel_base()
              : count_(0)
            {}

            virtual ~channel_base() {}

            virtual hpx::future<T> get(std::size_t generation) = 0;
            virtual void set(std::size_t generation, T && t) = 0;
            virtual void close() = 0;

            long use_count() const { return count_; }
            long addref() { return ++count_; }
            long release() { return --count_; }

        private:
            hpx::util::atomic_count count_;
        };

        template <typename T>
        void intrusive_ptr_add_ref(channel_base<T>* p)
        {
            p->addref();
        }

        template <typename T>
        void intrusive_ptr_release(channel_base<T>* p)
        {
            if (0 == p->release())
                delete p;
        }

        ///////////////////////////////////////////////////////////////////////
        template <typename T>
        class unlimited_channel : public channel_base<T>
        {
            typedef hpx::lcos::local::spinlock mutex_type;

            HPX_NON_COPYABLE(unlimited_channel);

        public:
            unlimited_channel()
              : closed_(false)
            {}

        protected:
            hpx::future<T> get(std::size_t generation)
            {
                std::unique_lock<mutex_type> l(mtx_);

                if (buffer_.empty() && closed_)
                {
                    l.unlock();
                    return hpx::make_exceptional_future<T>(
                        HPX_GET_EXCEPTION(hpx::invalid_status,
                            "hpx::lcos::local::channel::get",
                            "this channel is empty and was closed"));
                }

                ++get_generation_;
                if (generation == std::size_t(-1))
                    generation = get_generation_;

                return buffer_.receive(generation);
            }

            void set(std::size_t generation, T && t)
            {
                std::lock_guard<mutex_type> l(mtx_);

                ++set_generation_;
                if (generation == std::size_t(-1))
                    generation = set_generation_;

                buffer_.store_received(generation, std::move(t));
            }

            void close()
            {
                std::lock_guard<mutex_type> l(mtx_);
                closed_ = true;
            }

        private:
            mutable mutex_type mtx_;
            receive_buffer<T, no_mutex> buffer_;
            std::size_t get_generation_;
            std::size_t set_generation_;
            bool closed_;
        };
    }

    ///////////////////////////////////////////////////////////////////////////
    template <typename T = void> class receive_channel;
    template <typename T = void> class send_channel;
    template <typename T = void> class channel;

    ///////////////////////////////////////////////////////////////////////////
    template <typename T>
    class channel_iterator
      : public hpx::util::iterator_facade<
            channel_iterator<T>, T const, std::forward_iterator_tag>
    {
        typedef hpx::util::iterator_facade<
                channel_iterator<T>, T const, std::forward_iterator_tag
            > base_type;

    public:
        channel_iterator()
          : channel_(nullptr), data_(T(), false)
        {}

        inline channel_iterator(channel<T> const* c);
        inline channel_iterator(receive_channel<T> const* c);
        inline channel_iterator(send_channel<T> const* c);

    private:
        std::pair<T, bool> get_checked() const
        {
            hpx::future<T> f = channel_->get(std::size_t(-1));
            f.wait();

            if (f.has_exception())
                return std::make_pair(T(), false);
            return std::make_pair(f.get(), true);
        }

        friend class hpx::util::iterator_core_access;

        bool equal(channel_iterator const& rhs) const
        {
            return (channel_ == rhs.channel_ && data_.second == rhs.data_.second) ||
                !data_.second && rhs.channel_ == nullptr ||
                channel_ == nullptr && !rhs.data_.second;
        }

        void increment()
        {
            if (channel_)
                data_ = get_checked();
        }

        typename base_type::reference dereference() const
        {
            HPX_ASSERT(data_.second);
            return data_.first;
        }

    private:
        boost::intrusive_ptr<detail::channel_base<T> > channel_;
        std::pair<T, bool> data_;
    };

    template <>
    class channel_iterator<void>
      : public hpx::util::iterator_facade<
            channel_iterator<void>, util::unused_type const,
            std::forward_iterator_tag>
    {
        typedef hpx::util::iterator_facade<
                channel_iterator<void>, util::unused_type const,
                std::forward_iterator_tag
            > base_type;

    public:
        channel_iterator()
          : channel_(nullptr), data_(false)
        {}

        inline channel_iterator(channel<void> const* c);
        inline channel_iterator(receive_channel<void> const* c);
        inline channel_iterator(send_channel<void> const* c);

    private:
        bool get_checked()
        {
            hpx::future<util::unused_type> f = channel_->get(std::size_t(-1));
            f.wait();
            return !f.has_exception();
        }

        friend class hpx::util::iterator_core_access;

        bool equal(channel_iterator const& rhs) const
        {
            return (channel_ == rhs.channel_ && data_ == rhs.data_) ||
                !data_ && rhs.channel_ == nullptr ||
                channel_ == nullptr && !rhs.data_;
        }

        void increment()
        {
            if (channel_)
                data_ = get_checked();
        }

        typename base_type::reference dereference() const
        {
            HPX_ASSERT(data_);
            return util::unused;
        }

    private:
        boost::intrusive_ptr<detail::channel_base<util::unused_type> > channel_;
        bool data_;
    };

    ///////////////////////////////////////////////////////////////////////////
    template <typename T>
    class channel
    {
    public:
        channel()
          : channel_(new detail::unlimited_channel<T>())
        {}

        hpx::future<T>
        get_async(std::size_t generation = std::size_t(-1)) const
        {
            return channel_->get(generation);
        }
        T get(std::size_t generation = std::size_t(-1)) const
        {
            return channel_->get(generation).get();
        }
        std::pair<T, bool>
        get_checked(std::size_t generation = std::size_t(-1)) const
        {
            hpx::future<T> f = channel_->get(generation);
            f.wait();

            if (f.has_exception())
                return std::make_pair(T(), false);
            return std::make_pair(f.get(), true);
        }

        void set(T val, std::size_t generation = std::size_t(-1))
        {
            channel_->set(generation, std::move(val));
        }

        void close()
        {
            channel_->close();
        }

        channel_iterator<T> begin() const
        {
            return channel_iterator<T>(this);
        }
        channel_iterator<T> end() const
        {
            return channel_iterator<T>();
        }

        channel_iterator<T> rbegin() const
        {
            return channel_iterator<T>(this);
        }
        channel_iterator<T> rend() const
        {
            return channel_iterator<T>();
        }

    private:
        friend class channel_iterator<T>;
        friend class receive_channel<T>;
        friend class send_channel<T>;

    private:
        boost::intrusive_ptr<detail::channel_base<T> > channel_;
    };

    ///////////////////////////////////////////////////////////////////////////
    template <typename T>
    class receive_channel
    {
    public:
        receive_channel(channel<T> c)
          : channel_(c.channel_)
        {}

        hpx::future<T>
        get_async(std::size_t generation = std::size_t(-1)) const
        {
            return channel_->get(generation);
        }
        T get(std::size_t generation = std::size_t(-1)) const
        {
            return channel_->get(generation).get();
        }

        std::pair<T, bool>
        get_checked(std::size_t generation = std::size_t(-1)) const
        {
            hpx::future<T> f = channel_->get(generation);
            f.wait();

            if (f.has_exception())
                return std::make_pair(T(), false);
            return std::make_pair(f.get(), true);
        }

        channel_iterator<T> begin() const
        {
            return channel_iterator<T>(this);
        }
        channel_iterator<T> end() const
        {
            return channel_iterator<T>();
        }

        channel_iterator<T> rbegin() const
        {
            return channel_iterator<T>(this);
        }
        channel_iterator<T> rend() const
        {
            return channel_iterator<T>();
        }

    private:
        friend class channel_iterator<T>;

    private:
        boost::intrusive_ptr<detail::channel_base<T> > channel_;
    };

    ///////////////////////////////////////////////////////////////////////////
    template <typename T>
    class send_channel
    {
    public:
        send_channel(channel<T> c)
          : channel_(c.channel_)
        {}

        void set(T val, std::size_t generation = std::size_t(-1))
        {
            channel_->set(generation, std::move(val));
        }

        channel_iterator<T> begin() const
        {
            return channel_iterator<T>(this);
        }
        channel_iterator<T> end() const
        {
            return channel_iterator<T>();
        }

        channel_iterator<T> rbegin() const
        {
            return channel_iterator<T>(this);
        }
        channel_iterator<T> rend() const
        {
            return channel_iterator<T>();
        }

    private:
        friend class channel_iterator<T>;

    private:
        boost::intrusive_ptr<detail::channel_base<T> > channel_;
    };

    template <typename T>
    inline channel_iterator<T>::channel_iterator(channel<T> const* c)
      : channel_(c ? c->channel_ : nullptr),
        data_(c ? get_checked() : std::make_pair(T(), false))
    {}

    template <typename T>
    inline channel_iterator<T>::channel_iterator(receive_channel<T> const* c)
      : channel_(c ? c->channel_ : nullptr),
        data_(c ? get_checked() : std::make_pair(T(), false))
    {}

    template <typename T>
    inline channel_iterator<T>::channel_iterator(send_channel<T> const* c)
      : channel_(c ? c->channel_ : nullptr),
        data_(c ? get_checked() : std::make_pair(T(), false))
    {}

    ///////////////////////////////////////////////////////////////////////////
    template <>
    class channel<void>
    {
    public:
        channel()
          : channel_(new detail::unlimited_channel<util::unused_type>())
        {}

        hpx::future<void>
        get_async(std::size_t generation = std::size_t(-1)) const
        {
            return channel_->get(generation);
        }
        void get(std::size_t generation = std::size_t(-1)) const
        {
            channel_->get(generation).get();
        }
        bool get_checked(std::size_t generation = std::size_t(-1)) const
        {
            hpx::future<util::unused_type> f = channel_->get(generation);
            f.wait();
            return !f.has_exception();
        }

        void set(std::size_t generation = std::size_t(-1))
        {
            channel_->set(generation, hpx::util::unused_type());
        }

        void close()
        {
            channel_->close();
        }

    private:
        friend class channel_iterator<void>;
        friend class receive_channel<void>;
        friend class send_channel<void>;

    private:
        boost::intrusive_ptr<detail::channel_base<util::unused_type> > channel_;
    };

    ///////////////////////////////////////////////////////////////////////////
    template <>
    class receive_channel<void>
    {
    public:
        receive_channel(channel<void> const& c)
          : channel_(c.channel_)
        {}

        hpx::future<void>
        get_async(std::size_t generation = std::size_t(-1)) const
        {
            return channel_->get(generation);
        }
        void get(std::size_t generation = std::size_t(-1)) const
        {
            channel_->get(generation).get();
        }
        bool get_checked(std::size_t generation = std::size_t(-1)) const
        {
            hpx::future<util::unused_type> f = channel_->get(generation);
            f.wait();
            return !f.has_exception();
        }

    private:
        friend class channel_iterator<void>;

    private:
        boost::intrusive_ptr<detail::channel_base<util::unused_type> > channel_;
    };

    ///////////////////////////////////////////////////////////////////////////
    template <>
    class send_channel<void>
    {
    public:
        send_channel(channel<void> const& c)
          : channel_(c.channel_)
        {}

        void set(std::size_t generation = std::size_t(-1))
        {
            channel_->set(generation, util::unused_type());
        }

        void close()
        {
            channel_->close();
        }

    private:
        friend class channel_iterator<void>;

    private:
        boost::intrusive_ptr<detail::channel_base<util::unused_type> > channel_;
    };

    inline channel_iterator<void>::channel_iterator(channel<void> const* c)
      : channel_(c ? c->channel_ : nullptr),
        data_(c ? get_checked() : false)
    {}

    inline channel_iterator<void>::channel_iterator(receive_channel<void> const* c)
      : channel_(c ? c->channel_ : nullptr),
        data_(c ? get_checked() : false)
    {}

    inline channel_iterator<void>::channel_iterator(send_channel<void> const* c)
      : channel_(c ? c->channel_ : nullptr),
        data_(c ? get_checked() : false)
    {}
}}}

#endif
