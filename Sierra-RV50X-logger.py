
import requests
import json
import datetime
import time
import socket

# OUT_FILE = 'data.txt'
# This simple application logs the Sierra RV50X (possibly other Sierra modems too)
# The data is echoed to 127.0.0.1:35000, the SCReAM wrappers can get the data this
#  way and then include it the SCReAM logging
# >python3 Sierra-RV50X-logger.py


base_url = 'http://192.168.13.31:9191'
username = 'user'
password = '12345'

login_url = '{}/xml/Connect.xml'.format(base_url)
login_payload = '''<request xmlns="urn:acemanager">
<connect>
<login>{}</login>
<password><![CDATA[{}]]\x3e</password>
</connect>
</request>'''.format(username, password)

req_url = '{}/cgi-bin/Embedded_Ace_Get_Task.cgi'.format(base_url)
req_headers = {
    'Content-Type': 'text/xml',
}

# Param name to id map
param_ids = {
    'Cellular IP Address': 303,
    'ESN/EID/IMEI': 10,
    'Active Frequency Band': 671,
    'SIM ID': 771,
    'Cell ID': 773,
    'RSRP': 10210,
    'SINR': 10209,
    'RSSI': 261,
}

param_names = dict(map(lambda tup: (tup[1], tup[0]), param_ids.items()))

# payload = '303,12001,12002,10,771,11202,10701,10702,10704,785,773,774,775,671,674,672,675,1105,1104,10230,10298,5030,1082,1083,12005,12006,1091,1136,2753,5046,283,284,10283,10284,281,282,51006,52007,52008,53000,53101,53200,12003,12003,12003,12003'
#payload = '303,12001,10'
payload = '10210,10209,261,773'

def make_params_payload(params):
    param_list = map(lambda param: str(param_ids[param]), params)
    return ','.join(param_list)

def parse_pair(pair):
    [pid_str, value] = pair.split('=')
    pid = int(pid_str)
    param_name = param_names[pid] if pid in param_names else 'param {}'.format(pid)
    return (param_name, value)

def parse_params(data):
    pairs = data.split('!')[:-1]
    return dict(map(lambda pair: parse_pair(pair), pairs))

def make_request(params):
    with requests.Session() as s:
        p = s.post(login_url, data=login_payload)
        print('login response text:', p.text)

        #payload = make_params_payload(params)

        r = s.post(req_url, headers=req_headers, data=payload)
        # print('request text:', r.text)

        return {
            'timestamp': datetime.datetime.now().isoformat(),
            'data': parse_params(r.text),
        }
        # return None

def send_to_udp_socket(result):
    UDP_IP = "127.0.0.1"
    UDP_PORT = 35000
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.sendto(bytes(result, 'utf-8'), (UDP_IP, UDP_PORT)) 

# params = ['Cellular IP Address', 'ESN/EID/IMEI']
params = param_ids.keys()
try:
    while True:    
        result = make_request(params)
        #result = make_request(payload)
        #print('result:', json.dumps(result, indent=4))
        print(result)
        send_to_udp_socket(json.dumps(result))
        time.sleep(1)
except KeyboardInterrupt:
    print('\n Terminated from keyboard. \n')


# with open(OUT_FILE, 'a') as f:
#     f.write('row\n')
