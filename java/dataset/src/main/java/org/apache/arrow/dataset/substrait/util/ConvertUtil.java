/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package org.apache.arrow.dataset.substrait.util;

import java.util.Map;

import io.substrait.proto.Expression;

public class ConvertUtil {

  /**
   * Convert map to substrait Expression.
   *
   * @return Substrait Expression
   */
  public static Expression.Literal mapToExpressionLiteral(Map<String, String> values) {
    Expression.Literal.Builder literalBuilder = Expression.Literal.newBuilder();
    Expression.Literal.Map.KeyValue.Builder keyValueBuilder =
        Expression.Literal.Map.KeyValue.newBuilder();
    Expression.Literal.Map.Builder mapBuilder = Expression.Literal.Map.newBuilder();
    for (Map.Entry<String, String> entry : values.entrySet()) {
      literalBuilder.setString(entry.getKey());
      keyValueBuilder.setKey(literalBuilder.build());
      literalBuilder.setString(entry.getValue());
      keyValueBuilder.setValue(literalBuilder.build());
      mapBuilder.addKeyValues(keyValueBuilder.build());
    }
    literalBuilder.setMap(mapBuilder.build());
    return literalBuilder.build();
  }
}
