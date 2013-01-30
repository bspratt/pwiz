﻿/*
 * Original author: Nicholas Shulman <nicksh .at. u.washington.edu>,
 *                  MacCoss Lab, Department of Genome Sciences, UW
 *
 * Copyright 2011 University of Washington - Seattle, WA
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

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Threading;
using pwiz.Common.Collections;

namespace pwiz.Common.DataBinding.RowSources
{
    public class AbstractQuery
    {
        protected QueryResults RunAll(Pivoter.TickCounter tickCounter, QueryResults results)
        {
            results = Pivot(tickCounter, results);
            results = results.SetFilteredRows(Filter(tickCounter.CancellationToken, results));
            results = results.SetSortedRows(Sort(tickCounter.CancellationToken, results));
            return results;
        }

        protected QueryResults Pivot(Pivoter.TickCounter tickCounter, QueryResults results)
        {
            var pivoter = new Pivoter(results.Parameters.ViewInfo);
            var rowItems = ImmutableList.ValueOf(pivoter.ExpandAndPivot(tickCounter, results.SourceRows));
            return results.SetPivotedRows(pivoter, rowItems);
        }

        protected IEnumerable<RowItem> Filter(CancellationToken cancellationToken, QueryResults results)
        {
            var unfilteredRows = results.PivotedRows;
            var filter = results.Parameters.RowFilter;
            if (filter.IsEmpty)
            {
                return unfilteredRows;
            }
            var properties = results.ItemProperties;
            var filteredRows = new List<RowItem>();
            // toString on an enum is incredibly slow, so we cache the results in 
            // in a dictionary.
            var toStringCaches = new Dictionary<object, string>[properties.Count];
            for (int i = 0; i < properties.Count; i++)
            {
                var property = properties[i];
                if (property.PropertyType.IsEnum)
                {
                    toStringCaches[i] = new Dictionary<object, string>();
                }
            }

            foreach (var row in unfilteredRows)
            {
                for (int iProperty = 0; iProperty < properties.Count; iProperty++)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    var property = properties[iProperty];
                    var value = property.GetValue(row);
                    if (value == null)
                    {
                        continue;
                    }
                    var cache = toStringCaches[iProperty];
                    string strValue;
                    if (cache == null)
                    {
                        strValue = value.ToString();
                    }
                    else
                    {
                        if (!cache.TryGetValue(value, out strValue))
                        {
                            strValue = value.ToString();
                            cache.Add(value, strValue);
                        }
                    }
                    if (filter.Matches(strValue))
                    {
                        filteredRows.Add(row);
                        break;
                    }
                }
            }
            if (filteredRows.Count == unfilteredRows.Count)
            {
                return unfilteredRows;
            }
            return filteredRows;
        }
        protected IEnumerable<RowItem> Sort(CancellationToken cancellationToken, QueryResults results)
        {
            var sortDescriptions = results.Parameters.SortDescriptions;
            var unsortedRows = results.FilteredRows;
            if (sortDescriptions == null || sortDescriptions.Count == 0)
            {
                return unsortedRows;
            }
            var sortRows = new SortRow[unsortedRows.Count];
            for (int iRow = 0; iRow < sortRows.Count(); iRow++)
            {
                sortRows[iRow] = new SortRow(cancellationToken, results.Parameters, unsortedRows[iRow], iRow);
            }
            Array.Sort(sortRows);
            return Array.AsReadOnly(sortRows.Select(sr => sr.RowItem).ToArray());
        }
        class SortRow : IComparable<SortRow>
        {
            private object[] _keys;
            public SortRow(CancellationToken cancellationToken, QueryParameters queryParameters, RowItem rowItem, int rowIndex)
            {
                CancellationToken = cancellationToken;
                QueryParameters = queryParameters;
                RowItem = rowItem;
                OriginalRowIndex = rowIndex;
                _keys = new object[Sorts.Count];
                for (int i = 0; i < Sorts.Count; i++)
                {
                    _keys[i] = Sorts[i].PropertyDescriptor.GetValue(RowItem);
                }
            }
            public CancellationToken CancellationToken { get; private set; }
            public QueryParameters QueryParameters { get; private set; }
            public RowItem RowItem { get; private set; }
            public int OriginalRowIndex { get; private set; }
            public ListSortDescriptionCollection Sorts
            {
                get { return QueryParameters.SortDescriptions; }
            }
            public int CompareTo(SortRow other)
            {
                CancellationToken.ThrowIfCancellationRequested();
                for (int i = 0; i < Sorts.Count; i++)
                {
                    var sort = Sorts[i];
                    int result = QueryParameters.ViewInfo.DataSchema.Compare(_keys[i], other._keys[i]);
                    if (sort.SortDirection == ListSortDirection.Descending)
                    {
                        result = -result;
                    }
                    if (result != 0)
                    {
                        return result;
                    }
                }
                return OriginalRowIndex.CompareTo(other.OriginalRowIndex);
            }
        }
    }
}
