#ifndef VIVID_DISPLAY_CONSUMER_RECEIVER_H
#define VIVID_DISPLAY_CONSUMER_RECEIVER_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define VIVID_DISPLAY_CONSUMER_TYPE_RECEIVER \
    (vivid_display_consumer_receiver_get_type())

G_DECLARE_FINAL_TYPE(VividDisplayConsumerReceiver,
                     vivid_display_consumer_receiver,
                     VIVID_DISPLAY_CONSUMER,
                     RECEIVER,
                     GObject)

VividDisplayConsumerReceiver*
vivid_display_consumer_receiver_new(GSocketConnection* connection);

gboolean vivid_display_consumer_receiver_start(VividDisplayConsumerReceiver* self);
void     vivid_display_consumer_receiver_stop(VividDisplayConsumerReceiver* self);
gboolean vivid_display_consumer_receiver_get_running(VividDisplayConsumerReceiver* self);

G_END_DECLS

#endif
