// SPDX-License-Identifier: MIT
package org.keel.demo;
final class ApiError extends RuntimeException {
    final int status;
    ApiError(int status,String message) { super(message); this.status=status; }
}
