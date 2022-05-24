/******************************************************************************
 * VncEGLFS - Copyright (C) 2022 Uwe Rathmann
 * This file may be used under the terms of the 3-clause BSD License
 *****************************************************************************/

#include "VncServer.h"
#include "VncClient.h"

#include <qtcpserver.h>
#include <qopenglcontext.h>
#include <qopenglfunctions.h>
#include <qwindow.h>
#include <qthread.h>

#include <qpa/qplatformcursor.h>

namespace
{
    /*
        Often EGLFS is in combination with a touch screen, where you do not
        have a cursor and all we need is a dummy cursor so that we can make
        use of the mouse in the VNC client.

        But when having a cursor, it might be updated by an OpenGl shader,
        - like Qt::WaitCursor, that is rotating constantly.

        We have to find out how to deal with this all, but for the moment
        we simply go with a workaround, that acts like when having
        static cursor images.
     */
    VncCursor createCursor( Qt::CursorShape shape )
    {
        QPlatformCursorImage platformImage( nullptr, nullptr, 0, 0, 0, 0 );
        platformImage.set( shape );

        return { *platformImage.image(), platformImage.hotspot() };
    }

#if 0
    VncCursor createCursor( const QCursor* cursor )
    {
        const auto shape = cursor ? cursor->shape() : Qt::ArrowCursor;

        if ( shape == Qt::BitmapCursor )
            return { cursor->pixmap().toImage(), cursor->hotSpot() };

        return createCursor( shape );
    }
#endif

    class TcpServer final : public QTcpServer
    {
        Q_OBJECT

      public:
        TcpServer( QObject* parent )
            : QTcpServer( parent )
        {
        }

      Q_SIGNALS:
        void connectionRequested( qintptr );

      protected:
        void incomingConnection( qintptr socketDescriptor ) override
        {
            /*
                We do not want to use QTcpServer::nextPendingConnection to avoid
                QTcpSocket being created in the wrong thread
             */
            Q_EMIT connectionRequested( socketDescriptor );
        }
    };

    class ClientThread : public QThread
    {
      public:
        ClientThread( qintptr socketDescriptor, VncServer* server )
            : QThread( server )
            , m_socketDescriptor( socketDescriptor )
        {
        }

        ~ClientThread()
        {
        }

        void markDirty()
        {
            if ( m_client )
                m_client->markDirty();
        }

      protected:
        void run() override
        {
            VncClient client( m_socketDescriptor, qobject_cast< VncServer* >( parent() ) );
            connect( &client, &VncClient::disconnected, this, &QThread::quit );

            m_client = &client;
            QThread::run();
            m_client = nullptr;
        }

      private:
        VncClient* m_client = nullptr;
        const qintptr m_socketDescriptor;
    };
}

VncServer::VncServer( int port, QWindow* window )
    : m_window( window )
    , m_cursor( createCursor( Qt::ArrowCursor ) )
{
    Q_ASSERT( window && window->inherits( "QQuickWindow" ) );

    m_window = window;

    auto tcpServer = new TcpServer( this );
    connect( tcpServer, &TcpServer::connectionRequested, this, &VncServer::addClient );

    if( tcpServer->listen( QHostAddress::Any, port ) )
        qInfo( "VncServer created on port %d", port);
}

VncServer::~VncServer()
{
    m_window = nullptr;

    for ( auto thread : qAsConst( m_threads ) )
    {
        thread->quit();
        thread->wait( 20 );
    }
}

void VncServer::addClient( qintptr fd )
{
    auto thread = new ClientThread( fd, this );
    m_threads += thread;

    if ( m_window && !m_grabConnectionId )
    {
        /*
            frameSwapped is from the scene graph thread, so we
            need a Qt::DirectConnection to avoid, that the image is
            already gone, when being scheduled from a Qt::QQueuedConnection !
         */

        m_grabConnectionId = QObject::connect( m_window, SIGNAL(frameSwapped()),
            this, SLOT(updateFrameBuffer()), Qt::DirectConnection );
        
        QMetaObject::invokeMethod( m_window, "update" );
    }

    if ( auto tcpServer = qobject_cast< const QTcpServer* >( sender() ) )
    {
        qInfo() << "New VNC client attached on port" << tcpServer->serverPort()
            << ", #clients" << m_threads.count();
    }

    connect( thread, &QThread::finished, this, &VncServer::removeClient );
    thread->start();
}

void VncServer::removeClient()
{
    if ( auto thread = qobject_cast< QThread* >( sender() ) )
    {
        m_threads.removeOne( thread );
        if ( m_threads.isEmpty() && m_grabConnectionId )
            QObject::disconnect( m_grabConnectionId );

        thread->quit();
        thread->wait( 100 );

        delete thread;

        qInfo() << "VNC client detached, #clients:" << m_threads.count();
    }
}

static void grabWindow( QImage& frameBuffer )
{
#if 0
    #ifndef GL_BGRA
        #define GL_BGRA 0x80E1
    #endif

    #ifndef GL_UNSIGNED_INT_8_8_8_8_REV
        #define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
    #endif

    QOpenGLContext::currentContext()->functions()->glReadPixels(
        0, 0, frameBuffer.width(), frameBuffer.height(),
        GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, frameBuffer.bits() );

    // OpenGL images are vertically flipped.
    frameBuffer = std::move( frameBuffer ).mirrored( false, true );

#else
    // avoiding native OpenGL calls

    extern QImage qt_gl_read_framebuffer(
        const QSize&, bool alpha_format, bool include_alpha );

    const auto format = frameBuffer.format();
    frameBuffer = qt_gl_read_framebuffer( frameBuffer.size(), false, false );
    frameBuffer.convertTo( format );
#endif
}

void VncServer::updateFrameBuffer()
{
    {
        QMutexLocker locker( &m_frameBufferMutex );

        const auto size = m_window->size() * m_window->devicePixelRatio();
        if ( size != m_frameBuffer.size() )
        {
            /*
                On EGLFS the window always matches the screen size.

                But when testing the implementation on X11 the window
                might be resized manually later. Should be no problem,
                as most clients indicate being capable of adjustments
                of the framebuffer size. ( "DesktopSize" pseudo encoding )
             */

            m_frameBuffer = QImage( size, QImage::Format_RGB32 );
        }

        grabWindow( m_frameBuffer );
    }

    const QRect rect( 0, 0, m_frameBuffer.width(), m_frameBuffer.height() );

    for ( auto thread : qAsConst( m_threads ) )
    {
        auto clientThread = static_cast< ClientThread* >( thread );
        clientThread->markDirty();
    }
}

QWindow* VncServer::window() const
{
    return m_window;
}

QImage VncServer::frameBuffer() const
{
    QMutexLocker locker( &m_frameBufferMutex );
    const auto fb = m_frameBuffer;

    return fb;
}

VncCursor VncServer::cursor() const
{
    return m_cursor;
}

#include "VncServer.moc"
#include "moc_VncServer.cpp"
