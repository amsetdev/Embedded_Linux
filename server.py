from flask import Flask, request

app = Flask(__name__)

@app.route("/")
def home():
    return "Server Running"

@app.route("/sensor", methods=["GET"])
def get_sensor():
    return {
        "temperature": 30,
        "humidity": 65
    }

@app.route("/sensor", methods=["POST"])
def post_sensor():
    print("POST:", request.json)
    return {
        "status": "POST OK"
    }

@app.route("/sensor", methods=["PUT"])
def put_sensor():
    print("PUT:", request.json)
    return {
        "status": "PUT OK"
    }

@app.route("/sensor", methods=["DELETE"])
def delete_sensor():
    print("DELETE Request")
    return {
        "status": "DELETE OK"
    }

app.run(host="0.0.0.0", port=5000, ssl_context=("cert.pem", "key.pem"), threaded=True)
