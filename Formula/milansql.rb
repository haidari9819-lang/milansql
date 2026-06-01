class Milansql < Formula
  desc "Production-grade relational database engine — pure C++17, zero dependencies"
  homepage "https://haidari9819-lang.github.io/milansql/"
  url "https://github.com/haidari9819-lang/milansql/archive/v4.2.0.tar.gz"
  version "4.2.0"
  license "MIT"

  depends_on "cmake" => :build

  def install
    system "cmake", "-B", "build",
           "-DCMAKE_BUILD_TYPE=Release",
           "-DCMAKE_INSTALL_PREFIX=#{prefix}",
           *std_cmake_args
    system "cmake", "--build", "build", "--parallel"
    bin.install "build/milansql"
  end

  service do
    run [opt_bin/"milansql", "--server", "--port", "4406",
         "--http", "--http-port", "8080"]
    log_path var/"log/milansql.log"
    error_log_path var/"log/milansql.error.log"
  end

  test do
    system bin/"milansql", "--version"
  end
end
