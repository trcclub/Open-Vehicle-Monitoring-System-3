/*
;    Project:       Open Vehicle Monitor System
;    Date:          14th March 2017
;
;    Changes:
;    1.0  Initial release
;
;    (C) 2017  Mark Webb-Johnson
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in
; all copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
; THE SOFTWARE.
*/

#include "task_base.h"

Parent::Parent()
  {
  m_mutex = xSemaphoreCreateMutex();
  }

Parent::~Parent()
  {
  DeleteChildren();
  vSemaphoreDelete(m_mutex);
  }

void Parent::AddChild(TaskBase* child)
  {
  xSemaphoreTake(m_mutex, portMAX_DELAY);
  m_children.push_front(child);
  xSemaphoreGive(m_mutex);
  }

// This function would typically be called from the child's task to divorce
// itself from the parent because it is closing down on its own.
bool Parent::RemoveChild(TaskBase* child)
  {
  bool removed = false;
  xSemaphoreTake(m_mutex, portMAX_DELAY);
  Children::iterator at = m_children.begin(), after = at;
  if (at != m_children.end())
    {
    if (*at == child)
      {
      removed = true;
      m_children.pop_front();
      }
    else
      {
      for (++after; after != m_children.end(); ++at, ++after)
        {
        if (*after == child)
          {
          removed = true;
          m_children.erase_after(at);
          break;
          }
        }
      }
    }
  xSemaphoreGive(m_mutex);
  return removed;
  }

void Parent::DeleteChildren()
  {
  while (true)
    {
    xSemaphoreTake(m_mutex, portMAX_DELAY);
    if (m_children.empty())
      {
      xSemaphoreGive(m_mutex);
      break;
      }
    TaskBase* child = *m_children.begin();
    m_children.pop_front();
    xSemaphoreGive(m_mutex);
    child->DeleteTask();
    }
  }

TaskBase::TaskBase(Parent* parent)
  {
  m_parent = parent;
  }

TaskBase::~TaskBase()
  {
  }

void TaskBase::CreateTask(const char* name, int stack, UBaseType_t priority)
  {
  xTaskCreate(Task, name, stack, (void*)this, priority, &m_taskid);
  }

void TaskBase::CreateTaskPinned(const BaseType_t core, const char* name, int stack, UBaseType_t priority)
  {
  xTaskCreatePinnedToCore(Task, name, stack, (void*)this, priority, &m_taskid, core);
  }

void TaskBase::Task(void *object)
  {
  TaskBase* me = (TaskBase*)object;
  me->Service();
  me->DeleteFromParent();
  while (true); // Illegal instruction abort occurs if this function returns
  }

// This function should be overridden in the derived class to perform any
// cleanup operations that must be done before the task is deleted.
void TaskBase::Cleanup()
  {
  }

// This function must NOT be called from ~TaskBase().
void TaskBase::DeleteFromParent()
  {
  if (!m_parent || m_parent->RemoveChild(this))
    {
    TaskHandle_t taskid = m_taskid;
    delete this;
    vTaskDelete(taskid);
    }
  }

// This function must NOT be called from the TaskBase object's own task.
// It is only to be called from Parent::DeleteChildren() executing in the
// parent object's task.
void TaskBase::DeleteTask()
  {
  Cleanup();
  vTaskDelete(m_taskid);
  delete this;
  }