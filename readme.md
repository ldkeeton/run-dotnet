# run-dotnet

Run **dotnet** version for your project without installing it system-wide.  
By default it uses the latest **LTS**, or you can pin to a specific version.

## Build
```bash
./build.sh
```

## Usage
```bash
./run-dotnet run Program.cs      
./run-dotnet 10 run Program.cs   # pin to specific version
```

# Or use [github-exec](https://github.com/ldkeeton/github-exec) to run this as part of a one-liner.
```bash
github-exec ldkeeton/run-dotnet ef database update
```