var sys = require("sys"),  
my_http = require("http");  
my_http.createServer(function(request,response){  
    response.writeHeader(200, {"Content-Type": "text/plain"});  
    response.write("Hello World");  
    response.end();  
}).listen(2015);  
sys.puts("Server Running on 2015");


