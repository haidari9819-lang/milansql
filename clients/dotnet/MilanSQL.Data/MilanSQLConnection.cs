using System;
using System.Data;
using System.Data.Common;
using System.Net.Sockets;
using System.IO;
using System.Collections.Generic;

namespace MilanSQL.Data
{
    public class MilanSQLConnection : DbConnection
    {
        private string _connectionString;
        private TcpClient? _client;
        private StreamReader? _reader;
        private StreamWriter? _writer;
        private ConnectionState _state = ConnectionState.Closed;

        public MilanSQLConnection() { _connectionString = ""; }
        public MilanSQLConnection(string connectionString) { _connectionString = connectionString; }

        public override string ConnectionString
        {
            get => _connectionString;
            set => _connectionString = value ?? "";
        }

        public override string Database => ParseOption("Database", "public");
        public override string DataSource => ParseOption("Server", "localhost");
        public override string ServerVersion => "4.2.0";
        public override ConnectionState State => _state;

        private string ParseOption(string key, string defaultVal)
        {
            foreach (var part in _connectionString.Split(';'))
            {
                var kv = part.Split('=');
                if (kv.Length == 2 && kv[0].Trim().Equals(key, StringComparison.OrdinalIgnoreCase))
                    return kv[1].Trim();
            }
            return defaultVal;
        }

        public override void Open()
        {
            string host = ParseOption("Server", "localhost");
            int port = int.Parse(ParseOption("Port", "4406"));
            _client = new TcpClient(host, port);
            var stream = _client.GetStream();
            _reader = new StreamReader(stream);
            _writer = new StreamWriter(stream) { AutoFlush = true };
            _state = ConnectionState.Open;
        }

        public override void Close()
        {
            _client?.Close();
            _state = ConnectionState.Closed;
        }

        internal string[] SendQuery(string sql)
        {
            if (_state != ConnectionState.Open)
                throw new InvalidOperationException("Connection is not open.");
            _writer!.WriteLine("SQL_QUERY");
            _writer.WriteLine(sql);
            _writer.WriteLine("END");
            var lines = new List<string>();
            string? line;
            while ((line = _reader!.ReadLine()) != null)
            {
                if (line == "END") break;
                lines.Add(line);
            }
            return lines.ToArray();
        }

        protected override DbTransaction BeginDbTransaction(IsolationLevel isolationLevel)
        {
            SendQuery("BEGIN");
            return new MilanSQLTransaction(this, isolationLevel);
        }

        public override void ChangeDatabase(string databaseName) { }

        protected override DbCommand CreateDbCommand() => new MilanSQLCommand(this);

        public new MilanSQLCommand CreateCommand() => new MilanSQLCommand(this);

        protected override void Dispose(bool disposing) { if (disposing) Close(); base.Dispose(disposing); }
    }
}
