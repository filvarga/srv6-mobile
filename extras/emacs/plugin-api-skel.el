;;; plugin-api-skel.el - vpp engine plug-in "all-apih.c" skeleton
;;;
;;; Copyright (c) 2016 Cisco and/or its affiliates.
;;; Licensed under the Apache License, Version 2.0 (the "License");
;;; you may not use this file except in compliance with the License.
;;; You may obtain a copy of the License at:
;;;
;;;     http://www.apache.org/licenses/LICENSE-2.0
;;;
;;; Unless required by applicable law or agreed to in writing, software
;;; distributed under the License is distributed on an "AS IS" BASIS,
;;; WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
;;; See the License for the specific language governing permissions and
;;; limitations under the License.

(require 'skeleton)

(define-skeleton skel-plugin-api
"Insert a plug-in '<name>.api' skeleton "
nil
'(if (not (boundp 'plugin-name))
     (setq plugin-name (read-string "Plugin name: ")))
'(setq PLUGIN-NAME (upcase plugin-name))
"/*
 * " plugin-name ".api - binary API skeleton
 *
 * Copyright (c) <current-year> <your-organization>
 * Licensed under the Apache License, Version 2.0 (the \"License\");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an \"AS IS\" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file " plugin-name ".api
 * @brief VPP control-plane API messages.
 *
 * This file defines VPP control-plane binary API messages which are generally
 * called through a shared memory interface.
 */

/* Version and type recitations */

option version = \"0.1.0\";
import \"vnet/interface_types.api\";


/** @brief API to enable / disable " plugin-name " on an interface
    @param client_index - opaque cookie to identify the sender
    @param context - sender context, to match reply w/ request
    @param enable_disable - 1 to enable, 0 to disable the feature
    @param sw_if_index - interface handle
*/

autoreply define " plugin-name "_enable_disable {
    /* Client identifier, set from api_main.my_client_index */
    u32 client_index;

    /* Arbitrary context, so client can match reply to request */
    u32 context;

    /* Enable / disable the feature */
    bool enable_disable;

    /* Interface handle */
    vl_api_interface_index_t sw_if_index;
};
")
