using System;
using System.Collections;
using System.Collections.Generic;
using System.Data;
using System.Data.Common;

namespace MilanSQL.Data
{
    public class MilanSQLDataReader : DbDataReader
    {
        private readonly string[] _columns;
        private readonly List<string[]> _rows;
        private int _cursor = -1;

        private MilanSQLDataReader(string[] columns, List<string[]> rows)
        {
            _columns = columns;
            _rows = rows;
        }

        public static MilanSQLDataReader Parse(string[] lines, DbCommand cmd)
        {
            var dataLines = new List<string>();
            foreach (var l in lines)
            {
                if (string.IsNullOrWhiteSpace(l)) continue;
                if (l.StartsWith("+") || l.StartsWith("\u251C") || l.StartsWith("\u2514") || l.StartsWith("\u250C")) continue;
                if (l.StartsWith("OK") || l.StartsWith("ERROR")) continue;
                if (l.Contains("\u2502") || l.Contains("|") || l.Contains("\t")) dataLines.Add(l);
            }
            if (dataLines.Count == 0) return new MilanSQLDataReader(Array.Empty<string>(), new List<string[]>());

            string[] cols = SplitRow(dataLines[0]);
            var rows = new List<string[]>();
            for (int i = 1; i < dataLines.Count; i++)
                rows.Add(SplitRow(dataLines[i]));
            return new MilanSQLDataReader(cols, rows);
        }

        private static string[] SplitRow(string line)
        {
            var parts = line.Contains("\u2502") ? line.Split('\u2502') :
                        line.Contains("|") ? line.Split('|') :
                        line.Split('\t');
            var result = new List<string>();
            foreach (var p in parts) { var t = p.Trim(); if (t.Length > 0) result.Add(t); }
            return result.ToArray();
        }

        private string? Get(int i)
        {
            if (_cursor < 0 || _cursor >= _rows.Count) return null;
            var row = _rows[_cursor];
            if (i < 0 || i >= row.Length) return null;
            return row[i] == "NULL" ? null : row[i];
        }

        public override bool Read() { _cursor++; return _cursor < _rows.Count; }
        public override bool NextResult() => false;
        public override void Close() {}

        public override int FieldCount => _columns.Length;
        public override bool HasRows => _rows.Count > 0;
        public override bool IsClosed => false;
        public override int RecordsAffected => -1;
        public override int Depth => 0;

        public override string GetName(int i) => _columns.Length > i ? _columns[i] : $"col{i}";
        public override int GetOrdinal(string name) { for (int i = 0; i < _columns.Length; i++) if (_columns[i].Equals(name, StringComparison.OrdinalIgnoreCase)) return i; return -1; }
        public override object this[int i] => Get(i) ?? DBNull.Value;
        public override object this[string name] => Get(GetOrdinal(name)) ?? DBNull.Value;
        public override string GetDataTypeName(int i) => "TEXT";
        public override Type GetFieldType(int i) => typeof(string);
        public override object GetValue(int i) => Get(i) ?? DBNull.Value;
        public override int GetValues(object[] values) { for (int i = 0; i < Math.Min(values.Length, _columns.Length); i++) values[i] = GetValue(i); return Math.Min(values.Length, _columns.Length); }
        public override bool IsDBNull(int i) => Get(i) == null;
        public override bool GetBoolean(int i) { var v = Get(i); return v == "1" || "true".Equals(v, StringComparison.OrdinalIgnoreCase); }
        public override byte GetByte(int i) => byte.TryParse(Get(i), out var r) ? r : (byte)0;
        public override long GetBytes(int i, long o, byte[]? buf, int bo, int l) => 0;
        public override char GetChar(int i) { var v = Get(i); return v?.Length > 0 ? v[0] : '\0'; }
        public override long GetChars(int i, long o, char[]? buf, int co, int l) => 0;
        public override DateTime GetDateTime(int i) => DateTime.TryParse(Get(i), out var r) ? r : DateTime.MinValue;
        public override decimal GetDecimal(int i) => decimal.TryParse(Get(i), out var r) ? r : 0m;
        public override double GetDouble(int i) => double.TryParse(Get(i), out var r) ? r : 0.0;
        public override float GetFloat(int i) => float.TryParse(Get(i), out var r) ? r : 0f;
        public override Guid GetGuid(int i) => Guid.TryParse(Get(i), out var r) ? r : Guid.Empty;
        public override short GetInt16(int i) => short.TryParse(Get(i), out var r) ? r : (short)0;
        public override int GetInt32(int i) => int.TryParse(Get(i), out var r) ? r : 0;
        public override long GetInt64(int i) => long.TryParse(Get(i), out var r) ? r : 0L;
        public override string GetString(int i) => Get(i) ?? "";
        public override IEnumerator GetEnumerator() => _rows.GetEnumerator();
        public override DataTable? GetSchemaTable() => null;
    }
}
