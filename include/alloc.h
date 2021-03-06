/*
 * Copyright (c) 2009-2010 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _SUDO_ALLOC_H
#define _SUDO_ALLOC_H

#include <stdarg.h>

int	 easprintf(char **, const char *, ...) __printflike(2, 3);
int	 evasprintf(char **, const char *, va_list) __printflike(2, 0);
void	 efree(void *);
void	*ecalloc(size_t, size_t);
void	*emalloc(size_t);
void	*emalloc2(size_t, size_t);
void	*erealloc(void *, size_t);
void	*erealloc3(void *, size_t, size_t);
void	*erecalloc(void *, size_t, size_t, size_t);
char	*estrdup(const char *);
char	*estrndup(const char *, size_t);

#endif /* _SUDO_ALLOC_H */
