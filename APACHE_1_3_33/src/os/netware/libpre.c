/* Copyright 2000-2004 The Apache Software Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*------------------------------------------------------------------
  These functions are to be called when the shared NLM starts and
  stops.  By using these functions instead of defining a main()
  and calling ExitThread(TSR_THREAD, 0), the load time of the
  shared NLM is faster and memory size reduced.
   
  You may also want to override these in your own Apache module
  to do any cleanup other than the mechanism Apache modules
  provide.
------------------------------------------------------------------*/

int _lib_start();
int _lib_stop();

#ifdef __GNUC__
#include <string.h>        /* memset */
extern char _edata, _end ; /* end of DATA (start of BSS), end of BSS */
#endif

int _lib_start()
{
#ifdef __GNUC__
    memset (&_edata, 0, &_end - &_edata);
#endif
    return 0;
}

int _lib_stop()
{
    return 0;
}