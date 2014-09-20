/**************************************************************************
 *
 * Copyright 2014 Ilia Mirkin. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "main/imports.h"
#include "program/prog_parameter.h"
#include "program/prog_print.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "util/u_inlines.h"
#include "util/u_surface.h"

#include "st_debug.h"
#include "st_cb_bufferobjects.h"
#include "st_context.h"
#include "st_atom.h"
#include "st_program.h"

static void st_bind_atomics(struct st_context *st,
                           struct gl_shader_program *prog,
                           unsigned shader_type)
{
   unsigned i;

   if (!prog)
      return;

   for (i = 0; i < prog->NumAtomicBuffers; i++) {
      struct gl_atomic_buffer_binding *binding =
         &st->ctx->AtomicBufferBindings[prog->AtomicBuffers[i].Binding];
      struct st_buffer_object *st_obj =
         st_buffer_object(binding->BufferObject);
      if (!st_obj->surface ||
          st_obj->surface->u.buf.first_element != binding->Offset ||
          st_obj->surface->u.buf.last_element != st_obj->buffer->width0 - 1) {
         struct pipe_surface tmpl;

         u_surface_default_template(&tmpl, st_obj->buffer);
         tmpl.width = st_obj->buffer->width0 - binding->Offset;
         tmpl.height = 0;
         tmpl.u.buf.first_element = binding->Offset;
         tmpl.u.buf.last_element = st_obj->buffer->width0 - 1;

         if (st_obj->surface)
            pipe_surface_release(st->pipe, &st_obj->surface);
         st_obj->surface = st->pipe->create_surface(
               st->pipe, st_obj->buffer, &tmpl);
      }

      st->pipe->set_shader_resources(st->pipe, shader_type,
                                     i, 1, &st_obj->surface);
   }
}

static void bind_vs_atomics(struct st_context *st)
{
   struct gl_shader_program *prog =
      st->ctx->Shader.CurrentProgram[MESA_SHADER_VERTEX];

   st_bind_atomics(st, prog, PIPE_SHADER_VERTEX);
}

const struct st_tracked_state st_bind_vs_atomics = {
   "st_bind_vs_atomics",
   {
      0,
      ST_NEW_VERTEX_PROGRAM | ST_NEW_ATOMIC_BUFFER,
   },
   bind_vs_atomics
};

static void bind_fs_atomics(struct st_context *st)
{
   struct gl_shader_program *prog =
      st->ctx->Shader.CurrentProgram[MESA_SHADER_FRAGMENT];

   st_bind_atomics(st, prog, PIPE_SHADER_FRAGMENT);
}

const struct st_tracked_state st_bind_fs_atomics = {
   "st_bind_fs_atomics",
   {
      0,
      ST_NEW_FRAGMENT_PROGRAM | ST_NEW_ATOMIC_BUFFER,
   },
   bind_fs_atomics
};

static void bind_gs_atomics(struct st_context *st)
{
   struct gl_shader_program *prog =
      st->ctx->Shader.CurrentProgram[MESA_SHADER_GEOMETRY];

   st_bind_atomics(st, prog, PIPE_SHADER_GEOMETRY);
}

const struct st_tracked_state st_bind_gs_atomics = {
   "st_bind_gs_atomics",
   {
      0,
      ST_NEW_GEOMETRY_PROGRAM | ST_NEW_ATOMIC_BUFFER,
   },
   bind_gs_atomics
};
